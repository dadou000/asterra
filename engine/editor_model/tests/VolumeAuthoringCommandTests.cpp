#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-volume-command-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Volume Command Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(objects, schemas);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandRegistry registry;

        orbit::editor_model::authoring_commands::
            RegisterVolumeCommands(
                registry,
                commands,
                objects,
                selection);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto systemObject =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "System",
                worldObject);
        const auto bodyObject =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Body",
                systemObject);

        const orbit::scene::ObjectId selectedBody[]{
            bodyObject};
        selection.Set(selectedBody);

        orbit::commands::CommandArguments args;
        args.emplace(
            "preset",
            std::string{"Fire"});

        registry.Invoke(
            orbit::editor_model::authoring_commands::
                kCreateVolume,
            args);

        if (selection.Ordered().size() != 1U)
            return 1;

        const auto volume =
            selection.Ordered().front();

        const auto resolved =
            orbit::world_model::
                ResolveVolumeDomain(
                    objects,
                    volume);

        if (!resolved.has_value() ||
            resolved->preset != "Fire" ||
            resolved->solverPolicy !=
                orbit::world_model::
                    VolumeSolverPolicy::Local3D ||
            (resolved->fieldMask &
             static_cast<orbit::u64>(
                 orbit::world_model::
                     VolumeField::Emission)) == 0U)
            return 2;

        const auto schema =
            schemas.FindType(
                orbit::world_model::kVolumeType);

        if (schema == nullptr ||
            schema->properties.size() < 8U)
            return 3;

        if (!registry.Enablement(
                orbit::editor_model::
                    authoring_commands::
                        kRemoveVolume).enabled)
            return 4;

        registry.Invoke(
            orbit::editor_model::authoring_commands::
                kRemoveVolume);

        if (objects.Find(volume).has_value())
            return 5;

        if (!commands.CanUndo())
            return 6;

        commands.Undo();

        const auto restored =
            orbit::world_model::
                ResolveVolumeDomain(
                    objects,
                    volume);

        if (!restored.has_value() ||
            restored->preset != "Fire")
            return 7;

        world.Checkpoint();
    }

    // Reopen the same world and prove authored Volume state survives the
    // ordinary document persistence path rather than a volumetric sidecar.
    {
        auto project =
            orbit::documents::ProjectDocument::Open(
                root / "Project.orbit.toml");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::scene::ObjectStore objects(world);

        bool foundFire = false;

        for (const auto& rootObject :
             objects.Roots())
        {
            for (const auto& system :
                 objects.Children(rootObject.id))
            {
                for (const auto& body :
                     objects.Children(system.id))
                {
                    for (const auto& child :
                         objects.Children(body.id))
                    {
                        const auto resolved =
                            orbit::world_model::
                                ResolveVolumeDomain(
                                    objects,
                                    child.id);

                        if (resolved.has_value() &&
                            resolved->preset == "Fire")
                        {
                            foundFire = true;
                        }
                    }
                }
            }
        }

        if (!foundFire)
            return 8;
    }

    std::filesystem::remove_all(root);
    return 0;
}
