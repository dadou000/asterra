#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

int main()
{
    orbit::editor_model::OutputLog output(8);

    orbit::log::Info(
        "studio-output-sentinel");

    const auto entries =
        output.Snapshot();

    bool found = false;

    for (const auto& entry : entries)
    {
        if (entry.message ==
            "studio-output-sentinel")
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        return 1;
    }

    output.Clear();

    if (!output.Snapshot().empty())
    {
        return 2;
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-world-authoring-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    auto project =
        orbit::documents::ProjectDocument::Create(
            root,
            "World Authoring Test");

    orbit::documents::WorldDatabase world(
        project.StartupWorldPath());

    orbit::schema::SchemaRegistry schemas;
    orbit::editor_model::builtin::
        RegisterSchemas(schemas);

    const auto* bodySchema =
        schemas.FindType(
            orbit::editor_model::builtin::
                kCelestialBodyType);

    if (bodySchema == nullptr ||
        bodySchema->properties.size() < 10U)
    {
        return 3;
    }

    orbit::scene::ObjectStore objects(world);
    orbit::selection::SelectionService selection;
    orbit::commands::CommandService commandService(
        objects,
        schemas);
    orbit::commands::CommandRegistry registry;

    orbit::editor_model::
        authoring_commands::Register(
            registry,
            commandService,
            objects,
            selection);

    const auto worldObject =
        commandService.CreateObject(
            orbit::editor_model::builtin::
                kWorldType,
            "World");

    const std::array worldSelection{
        worldObject
    };
    selection.Set(
        std::span(worldSelection));

    if (!registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kCreateCelestialSystem).
            enabled ||
        !registry.Enablement(
            orbit::editor_model::
                authoring_commands::
                    kCreateCelestialBody).
            enabled)
    {
        return 4;
    }

    orbit::editor_model::
        CommandSurfaceRegistry surfaces;

    const auto explorerActions =
        surfaces.Present(
            "explorer",
            orbit::editor_model::
                CommandSurfaceKind::ContextMenu,
            registry);

    const auto propertyActions =
        surfaces.Present(
            "properties",
            orbit::editor_model::
                CommandSurfaceKind::Toolbar,
            registry);

    if (explorerActions.size() != 2U ||
        propertyActions.size() != 2U)
    {
        return 5;
    }

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::
                kCreateCelestialBody);

    if (selection.Ordered().size() != 1U)
    {
        return 6;
    }

    const auto bodyObject =
        selection.Ordered().front();
    const auto body =
        objects.Find(bodyObject);

    if (!body.has_value() ||
        body->type !=
            orbit::editor_model::builtin::
                kCelestialBodyType ||
        !body->parent.has_value())
    {
        return 7;
    }

    const auto system =
        objects.Find(*body->parent);

    if (!system.has_value() ||
        system->type !=
            orbit::editor_model::builtin::
                kCelestialSystemType ||
        system->parent !=
            std::optional<orbit::scene::ObjectId>(
                worldObject))
    {
        return 8;
    }

    const auto radius =
        objects.GetProperty(
            bodyObject,
            orbit::editor_model::builtin::
                kBodyRadius);
    const auto surfaceEnabled =
        objects.GetProperty(
            bodyObject,
            orbit::editor_model::builtin::
                kBodySurfaceEnabled);
    const auto atmosphereEnabled =
        objects.GetProperty(
            bodyObject,
            orbit::editor_model::builtin::
                kBodyAtmosphereEnabled);

    if (!radius.has_value() ||
        std::get<orbit::f64>(*radius) !=
            6'000'000.0 ||
        !surfaceEnabled.has_value() ||
        !std::get<bool>(*surfaceEnabled) ||
        !atmosphereEnabled.has_value() ||
        std::get<bool>(*atmosphereEnabled))
    {
        return 9;
    }

    const auto systemObject =
        *body->parent;

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::kUndo);

    if (objects.Find(bodyObject).has_value() ||
        objects.Find(systemObject).has_value())
    {
        return 10;
    }

    registry.Invoke(
        orbit::editor_model::
            authoring_commands::kRedo);

    if (!objects.Find(bodyObject).has_value() ||
        !objects.Find(systemObject).has_value())
    {
        return 11;
    }

    world.Checkpoint();
    std::filesystem::remove_all(root);
    return 0;
}
