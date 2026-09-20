#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-terrain-command-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Terrain Command Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(objects, schemas);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandRegistry registry;

        orbit::editor_model::authoring_commands::
            RegisterTerrainCommands(
                registry,
                commands,
                objects,
                selection);

        const auto worldObject = commands.CreateObject(
            orbit::world_model::kWorldType,
            "World");
        const auto systemObject = commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
        const auto bodyObject = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Asterra",
            systemObject);

        const orbit::scene::ObjectId bodySelection[] = {bodyObject};
        selection.Set(bodySelection);

        const auto before = registry.Enablement(
            orbit::editor_model::authoring_commands::kCreateTerrainSurface);

        if (!before.enabled)
        {
            return 1;
        }

        registry.Invoke(
            orbit::editor_model::authoring_commands::kCreateTerrainSurface);

        if (selection.Ordered().size() != 1U)
        {
            return 2;
        }

        const auto terrainObject = selection.Ordered().front();
        const auto terrain = objects.Find(terrainObject);

        if (!terrain.has_value() ||
            terrain->type != orbit::world_model::kTerrainSurfaceType ||
            terrain->parent != bodyObject)
        {
            return 3;
        }

        const auto terrainChildren =
            objects.Children(
                terrainObject);

        if (terrainChildren.size() != 1U ||
            terrainChildren.front().type !=
                orbit::world_model::
                    kTerrainProcessAssetType)
        {
            return 13;
        }

        const auto processObject =
            terrainChildren.front().id;

        const auto aeolianEnabled =
            objects.GetProperty(
                processObject,
                orbit::world_model::
                    kProcessAeolianEnabled);

        if (!aeolianEnabled.has_value() ||
            !std::get<bool>(
                *aeolianEnabled))
        {
            return 14;
        }

        const auto seed = objects.GetProperty(
            terrainObject,
            orbit::world_model::kTerrainSeed);
        const auto octaves = objects.GetProperty(
            terrainObject,
            orbit::world_model::kTerrainDetailOctaves);

        if (!seed.has_value() || !octaves.has_value() ||
            std::get<orbit::i64>(*seed) != 0x41535445525241LL ||
            std::get<orbit::i64>(*octaves) != 10)
        {
            return 4;
        }

        selection.Set(bodySelection);

        if (registry.Enablement(
                orbit::editor_model::authoring_commands::
                    kCreateTerrainSurface).enabled)
        {
            return 5;
        }

        if (!registry.Enablement(
                orbit::editor_model::authoring_commands::
                    kRemoveTerrainSurface).enabled)
        {
            return 6;
        }

        registry.Invoke(
            orbit::editor_model::authoring_commands::kRemoveTerrainSurface);

        if (objects.Find(terrainObject).has_value() ||
            objects.Find(processObject).has_value() ||
            selection.Ordered().size() != 1U ||
            selection.Ordered().front() != bodyObject)
        {
            return 7;
        }

        if (!commands.CanUndo())
        {
            return 8;
        }

        commands.Undo();

        const auto restored = objects.Find(terrainObject);
        const auto restoredSeed = objects.GetProperty(
            terrainObject,
            orbit::world_model::kTerrainSeed);

        if (!restored.has_value() ||
            restored->type != orbit::world_model::kTerrainSurfaceType ||
            !restoredSeed.has_value() ||
            std::get<orbit::i64>(*restoredSeed) != 0x41535445525241LL ||
            !objects.Find(processObject).has_value())
        {
            return 9;
        }

        if (!commands.CanRedo())
        {
            return 10;
        }

        commands.Redo();

        if (objects.Find(terrainObject).has_value() ||
            objects.Find(processObject).has_value())
        {
            return 11;
        }

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyEllipsoidEnabled,
            true);
        selection.Set(bodySelection);

        const auto ellipsoid = registry.Enablement(
            orbit::editor_model::authoring_commands::kCreateTerrainSurface);

        if (ellipsoid.enabled)
        {
            return 12;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
