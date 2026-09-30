#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/PrimitiveBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <exception>
#include <filesystem>

int main()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-primitive-binding-" +
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
            documents::ProjectDocument::Create(
                root,
                "Primitive Binding Test");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);

        check(schemas.FindType(
                  world_model::kPrimitiveType) != nullptr);

        scene::ObjectStore objects(world);
        commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                world_model::kWorldType,
                "World");
        const auto body =
            commands.CreateObject(
                world_model::kCelestialBodyType,
                "Body",
                worldObject);

        // A fresh primitive resolves with the schema defaults.
        const auto box =
            commands.CreateObject(
                world_model::kPrimitiveType,
                "Box",
                body);

        auto resolved =
            world_model::ResolvePrimitives(objects);
        check(resolved.size() == 1U);
        if (resolved.size() == 1U)
        {
            check(resolved[0].object == box);
            check(resolved[0].shape ==
                  world_model::PrimitiveShape::Box);
            check(resolved[0].sizeMeters.x == 1.0);
            check(resolved[0].castShadows);
            check(resolved[0].materialAsset.empty());
        }

        // Authored values flow through, including nested placement.
        const auto group =
            commands.CreateObject(
                world_model::kCelestialReferenceNodeType,
                "Group",
                body);
        const auto sphere =
            commands.CreateObject(
                world_model::kPrimitiveType,
                "Sphere",
                group);

        commands.SetProperty(
            sphere,
            world_model::kPrimitiveShape,
            i64{1});
        commands.SetProperty(
            sphere,
            world_model::kPrimitivePositionMeters,
            math::Double3{1.0, 2.0, 3.0});
        commands.SetProperty(
            sphere,
            world_model::kPrimitiveSizeMeters,
            math::Double3{4.0, 4.0, 4.0});
        commands.SetProperty(
            sphere,
            world_model::kPrimitiveMaterialAsset,
            std::string{"Materials/Rock"});

        resolved = world_model::ResolvePrimitives(objects);
        check(resolved.size() == 2U);

        resolved =
            world_model::ResolvePrimitives(objects, group);
        check(resolved.size() == 1U);
        if (resolved.size() == 1U)
        {
            check(resolved[0].shape ==
                  world_model::PrimitiveShape::Sphere);
            check(resolved[0].positionMeters.y == 2.0);
            check(resolved[0].sizeMeters.z == 4.0);
            check(resolved[0].materialAsset == "Materials/Rock");
        }

        // Disabled or malformed primitives are skipped.
        commands.SetProperty(
            sphere,
            world_model::kPrimitiveEnabled,
            false);
        check(world_model::ResolvePrimitives(objects).size() == 1U);

        commands.SetProperty(
            sphere,
            world_model::kPrimitiveEnabled,
            true);
        commands.SetProperty(
            sphere,
            world_model::kPrimitiveSizeMeters,
            math::Double3{1.0, 0.0, 1.0});
        check(world_model::ResolvePrimitives(objects).size() == 1U);

        commands.SetProperty(
            sphere,
            world_model::kPrimitiveSizeMeters,
            math::Double3{1.0, 1.0, 1.0});
        // Schema validation keeps out-of-range shapes from being stored.
        bool rejected = false;
        try
        {
            commands.SetProperty(
                sphere,
                world_model::kPrimitiveShape,
                i64{9});
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        check(rejected);
        check(world_model::ResolvePrimitives(objects).size() == 2U);
    }

    std::filesystem::remove_all(root);
    return failures;
}
