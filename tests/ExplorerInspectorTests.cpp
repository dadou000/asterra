#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "Explorer/Inspector test failed: " #expression \
                      << " at line " << __LINE__ << '\\n'; \
            return __LINE__; \
        } \
    } while (false)

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-editor-model-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Editor Model Test");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        orbit::schema::SchemaRegistry schemas;
        orbit::editor_model::builtin::
            RegisterSchemas(schemas);

        orbit::scene::ObjectStore objects(world);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::commands::CommandRegistry registry;

        orbit::editor_model::authoring_commands::
            Register(
                registry,
                commands,
                objects,
                selection);

        orbit::editor_model::ExplorerModel explorer(
            objects,
            commands,
            selection);

        orbit::editor_model::InspectorModel inspector(
            objects,
            schemas,
            commands,
            selection);

        const auto worldRoot =
            commands.CreateObject(
                orbit::editor_model::builtin::
                    kWorldType,
                "World");

        selection.Set(
            std::span(
                std::array{worldRoot}));

        ORBIT_TEST_CHECK(
            registry.Enablement(
                orbit::editor_model::
                    authoring_commands::
                        kCreateCelestialSystem).
                enabled);

        registry.Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateCelestialSystem);

        ORBIT_TEST_CHECK(
            selection.Ordered().size() == 1);

        const auto system =
            selection.Ordered().front();
        const auto systemRecord =
            objects.Find(system);

        ORBIT_TEST_CHECK(systemRecord.has_value());
        ORBIT_TEST_CHECK(
            systemRecord->type ==
            orbit::editor_model::builtin::
                kCelestialSystemType);
        ORBIT_TEST_CHECK(
            systemRecord->parent ==
            std::optional(worldRoot));

        ORBIT_TEST_CHECK(
            registry.Enablement(
                orbit::editor_model::
                    authoring_commands::
                        kCreateCelestialBody).
                enabled);

        registry.Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateCelestialBody);

        const auto bodyA =
            selection.Ordered().front();
        explorer.Rename(bodyA, "Asterra");

        selection.Set(
            std::span(
                std::array{system}));
        registry.Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateCelestialBody);

        const auto bodyB =
            selection.Ordered().front();
        explorer.Rename(bodyB, "Luma");

        ORBIT_TEST_CHECK(
            explorer.Roots().size() == 1);
        ORBIT_TEST_CHECK(
            explorer.Children(worldRoot).size() == 1);
        ORBIT_TEST_CHECK(
            explorer.Children(system).size() == 2);
        ORBIT_TEST_CHECK(
            explorer.Search("aster").size() == 1);

        explorer.Select(bodyA, false);
        explorer.Select(bodyB, true);

        ORBIT_TEST_CHECK(
            selection.Ordered().size() == 2);

        const auto properties =
            inspector.CommonProperties();

        bool foundRadius = false;
        bool foundMass = false;
        bool foundTilt = false;
        bool foundRotation = false;
        bool foundPosition = false;

        for (const auto& property : properties)
        {
            if (property.schema.id ==
                orbit::editor_model::builtin::
                    kBodyRadius)
            {
                foundRadius = true;
                ORBIT_TEST_CHECK(!property.mixed);
            }
            else if (property.schema.id ==
                orbit::editor_model::builtin::
                    kBodyMass)
            {
                foundMass = true;
            }
            else if (property.schema.id ==
                orbit::editor_model::builtin::
                    kBodyAxialTiltDegrees)
            {
                foundTilt = true;
            }
            else if (property.schema.id ==
                orbit::editor_model::builtin::
                    kBodyRotationPeriodSeconds)
            {
                foundRotation = true;
            }
            else if (property.schema.id ==
                orbit::editor_model::builtin::
                    kBodyParentPositionMeters)
            {
                foundPosition = true;
            }
        }

        ORBIT_TEST_CHECK(foundRadius);
        ORBIT_TEST_CHECK(foundMass);
        ORBIT_TEST_CHECK(foundTilt);
        ORBIT_TEST_CHECK(foundRotation);
        ORBIT_TEST_CHECK(foundPosition);

        inspector.SetForSelection(
            orbit::editor_model::builtin::
                kBodyRadius,
            7'000'000.0);

        ORBIT_TEST_CHECK(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    bodyA,
                    orbit::editor_model::builtin::
                        kBodyRadius)) ==
            7'000'000.0);

        ORBIT_TEST_CHECK(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    bodyB,
                    orbit::editor_model::builtin::
                        kBodyRadius)) ==
            7'000'000.0);

        // The inspector groups a multi-object edit into one undo unit.
        commands.Undo();

        ORBIT_TEST_CHECK(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    bodyA,
                    orbit::editor_model::builtin::
                        kBodyRadius)) ==
            6'000'000.0);

        ORBIT_TEST_CHECK(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    bodyB,
                    orbit::editor_model::builtin::
                        kBodyRadius)) ==
            6'000'000.0);

        commands.Redo();

        explorer.Rename(
            bodyA,
            "Asterra Prime");

        ORBIT_TEST_CHECK(
            objects.Find(bodyA)->name ==
            "Asterra Prime");

        commands.Undo();

        ORBIT_TEST_CHECK(
            objects.Find(bodyA)->name ==
            "Asterra");

        explorer.Reparent(
            bodyB,
            worldRoot);

        ORBIT_TEST_CHECK(
            objects.Find(bodyB)->parent ==
            std::optional(worldRoot));

        commands.Undo();

        ORBIT_TEST_CHECK(
            objects.Find(bodyB)->parent ==
            std::optional(system));

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
