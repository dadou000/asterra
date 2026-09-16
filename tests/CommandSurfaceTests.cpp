#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
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
        ("orbit-command-surfaces-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    auto project =
        orbit::documents::ProjectDocument::Create(
            root,
            "Command Surface Test");

    orbit::documents::WorldDatabase world(
        project.StartupWorldPath());

    orbit::schema::SchemaRegistry schemas;
    orbit::editor_model::builtin::
        RegisterSchemas(schemas);

    orbit::scene::ObjectStore objects(world);
    orbit::selection::SelectionService selection;

    orbit::commands::CommandService
        commandService(
            objects,
            schemas);

    const auto worldObject =
        commandService.CreateObject(
            orbit::editor_model::builtin::
                kWorldType,
            "World");

    const auto bodyObject =
        commandService.CreateObject(
            orbit::editor_model::builtin::
                kCelestialBodyType,
            "Asterra",
            worldObject);

    // Clear setup edits so the test can reason about command enablement.
    commandService.Undo();
    commandService.Undo();
    commandService.Redo();
    commandService.Redo();

    orbit::commands::CommandRegistry registry;

    orbit::editor_model::
        authoring_commands::Register(
            registry,
            commandService,
            objects,
            selection);

    orbit::editor_model::
        CommandSurfaceRegistry surfaces;

    surfaces.Set(
        "viewport",
        orbit::editor_model::
            CommandSurfaceKind::Toolbar,
        {
            orbit::editor_model::
                authoring_commands::kUndo,
            orbit::editor_model::
                authoring_commands::
                    kClearSelection
        });

    surfaces.Add(
        "viewport",
        orbit::editor_model::
            CommandSurfaceKind::Toolbar,
        orbit::editor_model::
            authoring_commands::
                kAssignMaterial);

    ORBIT_TEST_CHECK(
        surfaces.Commands(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar).
            size() == 3);

    ORBIT_TEST_CHECK(
        surfaces.Remove(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar,
            orbit::editor_model::
                authoring_commands::
                    kAssignMaterial));

    ORBIT_TEST_CHECK(
        surfaces.Commands(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar).
            size() == 2);

    surfaces.Set(
        "viewport",
        orbit::editor_model::
            CommandSurfaceKind::Radial,
        {
            orbit::editor_model::
                authoring_commands::
                    kMoveToRoot,
            orbit::editor_model::
                authoring_commands::
                    kClearSelection
        });

    surfaces.Set(
        "explorer",
        orbit::editor_model::
            CommandSurfaceKind::ContextMenu,
        {
            orbit::editor_model::
                authoring_commands::
                    kMoveToRoot
        });

    const auto noSelection =
        registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kMoveToRoot);

    ORBIT_TEST_CHECK(!noSelection.enabled);
    ORBIT_TEST_CHECK(
        !noSelection.reason.empty());

    const std::array selected{
        bodyObject
    };

    selection.Set(
        std::span(selected));

    const auto enabled =
        registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kMoveToRoot);

    ORBIT_TEST_CHECK(enabled.enabled);

    const auto materialEnabled =
        registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kAssignMaterial);

    ORBIT_TEST_CHECK(materialEnabled.enabled);

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::
                kAssignMaterial,
        {
            {
                "material",
                std::string(
                    "Content/Materials/steel.orbitmaterial")
            }
        });

    ORBIT_TEST_CHECK(
        std::get<std::string>(
            *objects.GetProperty(
                bodyObject,
                orbit::editor_model::builtin::
                    kBodyMaterialAsset)) ==
        "Content/Materials/steel.orbitmaterial");

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::kUndo);

    ORBIT_TEST_CHECK(
        !objects.GetProperty(
            bodyObject,
            orbit::editor_model::builtin::
                kBodyMaterialAsset).has_value());

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::kRedo);

    ORBIT_TEST_CHECK(
        std::get<std::string>(
            *objects.GetProperty(
                bodyObject,
                orbit::editor_model::builtin::
                    kBodyMaterialAsset)) ==
        "Content/Materials/steel.orbitmaterial");

    const auto radial =
        surfaces.Present(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Radial,
            registry);

    const auto context =
        surfaces.Present(
            "explorer",
            orbit::editor_model::
                CommandSurfaceKind::ContextMenu,
            registry);

    ORBIT_TEST_CHECK(radial.size() == 2);
    ORBIT_TEST_CHECK(context.size() == 1);
    ORBIT_TEST_CHECK(
        radial.front().id ==
        context.front().id);
    ORBIT_TEST_CHECK(
        radial.front().enabled);

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::
                kMoveToRoot);

    ORBIT_TEST_CHECK(
        objects.Find(bodyObject)->
            parent ==
        std::nullopt);

    const auto alreadyAtRoot =
        registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kMoveToRoot);

    ORBIT_TEST_CHECK(!alreadyAtRoot.enabled);
    ORBIT_TEST_CHECK(
        alreadyAtRoot.reason ==
        "The selected object is already at the root.");

    ORBIT_TEST_CHECK(
        registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kUndo).
            enabled);

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::kUndo);

    ORBIT_TEST_CHECK(
        objects.Find(bodyObject)->
            parent ==
        std::optional<
            orbit::scene::ObjectId>(
                worldObject));

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::
                kClearSelection);

    ORBIT_TEST_CHECK(
        selection.Ordered().empty());

    const auto toolbar =
        surfaces.Present(
            "viewport",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar,
            registry);

    bool foundDisabledClear = false;

    for (const auto& command : toolbar)
    {
        if (command.id ==
                orbit::editor_model::
                    authoring_commands::
                        kClearSelection &&
            !command.enabled &&
            !command.disabledReason.empty())
        {
            foundDisabledClear = true;
        }
    }

    ORBIT_TEST_CHECK(
        foundDisabledClear);

    std::filesystem::remove_all(root);
    return 0;
}
