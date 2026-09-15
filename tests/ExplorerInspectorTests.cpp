#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <array>
#include <filesystem>
#include <span>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
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

    const auto bodyA =
        commands.CreateObject(
            orbit::editor_model::builtin::
                kCelestialBodyType,
            "Asterra",
            worldRoot);

    const auto bodyB =
        commands.CreateObject(
            orbit::editor_model::builtin::
                kCelestialBodyType,
            "Luma",
            worldRoot);

    ORBIT_TEST_CHECK(
        explorer.Roots().size() == 1);

    ORBIT_TEST_CHECK(
        explorer.Children(worldRoot).size() == 2);

    ORBIT_TEST_CHECK(
        explorer.Search("aster").size() == 1);

    explorer.Select(bodyA, false);
    explorer.Select(bodyB, true);

    ORBIT_TEST_CHECK(
        selection.Ordered().size() == 2);

    const auto properties =
        inspector.CommonProperties();

    bool foundRadius = false;

    for (const auto& property : properties)
    {
        if (property.schema.id ==
            orbit::editor_model::builtin::
                kBodyRadius)
        {
            foundRadius = true;
            ORBIT_TEST_CHECK(!property.mixed);
            break;
        }
    }

    ORBIT_TEST_CHECK(foundRadius);

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
        !objects.GetProperty(
            bodyA,
            orbit::editor_model::builtin::
                kBodyRadius).has_value());

    ORBIT_TEST_CHECK(
        !objects.GetProperty(
            bodyB,
            orbit::editor_model::builtin::
                kBodyRadius).has_value());

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
        std::nullopt);

    ORBIT_TEST_CHECK(
        objects.Find(bodyB)->parent ==
        std::nullopt);

    commands.Undo();

    ORBIT_TEST_CHECK(
        objects.Find(bodyB)->parent ==
        std::optional<
            orbit::scene::ObjectId>(
                worldRoot));

    world.Checkpoint();
    std::filesystem::remove_all(root);
    return 0;
}
