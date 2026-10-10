#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/studio_ui/StudioSpawn.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>

int main()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-spawn-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project = documents::ProjectDocument::Create(root, "Spawn Test");
        documents::WorldDatabase world(project.StartupWorldPath());
        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);
        if (schemas.FindType(world_model::kSpawnLocationType) == nullptr)
        {
            return 1;
        }

        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);

        if (studio_ui::FindProjectSpawn(objects).has_value())
        {
            return 2;
        }

        const auto worldObject =
            commands.CreateObject(world_model::kWorldType, "World");
        const auto body = commands.CreateObject(
            world_model::kCelestialBodyType, "Body", worldObject);

        // Siblings resolve in ObjectStore order (sort_order, then name).
        const auto disabled = commands.CreateObject(
            world_model::kSpawnLocationType, "A Disabled", body);
        commands.SetProperty(
            disabled, world_model::kSpawnLocationEnabled, false);
        const auto active = commands.CreateObject(
            world_model::kSpawnLocationType, "B Active", body);
        commands.SetProperty(
            active,
            world_model::kSpawnLocationPositionMeters,
            math::Double3{1.0, 2.0, 3.0});
        commands.SetProperty(active, world_model::kSpawnLocationYawDegrees, 30.0);
        commands.SetProperty(active, world_model::kSpawnLocationPitchDegrees, -5.0);

        const auto all = studio_ui::ListSpawnLocations(objects);
        if (all.size() != 2U || all[0].enabled || !all[1].enabled)
        {
            return 3;
        }

        const auto project_spawn = studio_ui::FindProjectSpawn(objects);
        if (!project_spawn.has_value() || project_spawn->id != active ||
            project_spawn->body != body ||
            project_spawn->positionMeters.z != 3.0 ||
            project_spawn->yawDegrees != 30.0 ||
            project_spawn->pitchDegrees != -5.0 ||
            project_spawn->eyeHeightMeters != 2.0)
        {
            return 4;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
