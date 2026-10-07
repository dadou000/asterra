#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/StaticMeshBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <exception>
#include <filesystem>

int main()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-static-mesh-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    int failures = 0;
    const auto check =
        [&failures](const bool condition)
        {
            if (!condition)
            {
                ++failures;
            }
        };

    {
        auto project =
            documents::ProjectDocument::Create(root, "Static Mesh Test");
        documents::WorldDatabase world(project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);
        check(schemas.FindType(world_model::kStaticMeshType) != nullptr);

        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);

        const auto worldObject =
            commands.CreateObject(world_model::kWorldType, "World");
        const auto body =
            commands.CreateObject(
                world_model::kCelestialBodyType, "Body", worldObject);
        const auto mesh =
            commands.CreateObject(
                world_model::kStaticMeshType, "Palace", body);

        // A mesh with no asset yet draws nothing (and is not an error).
        check(world_model::ResolveStaticMeshes(objects).empty());

        commands.SetProperty(
            mesh,
            world_model::kStaticMeshAsset,
            std::string{"Content/Models/Palace/scene.glb"});
        commands.SetProperty(
            mesh,
            world_model::kStaticMeshPositionMeters,
            math::Double3{1.0, 2.0, 3.0});
        commands.SetProperty(
            mesh, world_model::kStaticMeshScale, 0.5);

        auto resolved = world_model::ResolveStaticMeshes(objects);
        check(resolved.size() == 1U);
        if (resolved.size() == 1U)
        {
            check(resolved[0].object == mesh);
            check(resolved[0].meshAsset == "Content/Models/Palace/scene.glb");
            check(resolved[0].positionMeters.z == 3.0);
            check(resolved[0].uniformScale == 0.5);
            check(resolved[0].castShadows);
        }
        check(world_model::ResolveStaticMeshes(objects, body).size() == 1U);

        commands.SetProperty(
            mesh, world_model::kStaticMeshEnabled, false);
        check(world_model::ResolveStaticMeshes(objects).empty());
        commands.SetProperty(
            mesh, world_model::kStaticMeshEnabled, true);

        // The schema rejects a non-positive scale instead of storing it.
        bool rejected = false;
        try
        {
            commands.SetProperty(
                mesh, world_model::kStaticMeshScale, 0.0);
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected);
        check(world_model::ResolveStaticMeshes(objects).size() == 1U);
    }

    std::filesystem::remove_all(root);
    return failures;
}
