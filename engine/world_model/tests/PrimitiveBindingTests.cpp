#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/PrimitiveBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <exception>
#include <cstdio>
#include <filesystem>
#include <source_location>

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
        [&failures](
            const bool condition,
            const std::source_location where =
                std::source_location::current())
        {
            if (!condition)
            {
                std::fprintf(
                    stderr, "PrimitiveBindingTests: check failed at line %u\n",
                    static_cast<unsigned>(where.line()));
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

        // Surface defaults, authored glass / mirror values and clamping.
        resolved = world_model::ResolvePrimitives(objects, group);
        check(resolved.size() == 1U);
        if (resolved.size() == 1U)
        {
            check(resolved[0].surface ==
                  world_model::PrimitiveSurface::Standard);
            check(resolved[0].indexOfRefraction == 1.5);
            check(resolved[0].caustics);
        }

        commands.SetProperty(
            sphere, world_model::kPrimitiveSurface, i64{2});
        commands.SetProperty(
            sphere, world_model::kPrimitiveIor, 1.33);
        commands.SetProperty(
            sphere,
            world_model::kPrimitiveColor,
            math::Double3{0.2, 0.9, 0.4});
        commands.SetProperty(
            sphere, world_model::kPrimitiveCaustics, false);
        resolved = world_model::ResolvePrimitives(objects, group);
        if (resolved.size() == 1U)
        {
            check(resolved[0].surface ==
                  world_model::PrimitiveSurface::Glass);
            check(resolved[0].indexOfRefraction == 1.33);
            check(resolved[0].color.y == 0.9);
            check(!resolved[0].caustics);
        }

        commands.SetProperty(
            sphere, world_model::kPrimitiveSurface, i64{1});
        resolved = world_model::ResolvePrimitives(objects, group);
        if (resolved.size() == 1U)
        {
            check(resolved[0].surface ==
                  world_model::PrimitiveSurface::Mirror);
        }

        bool surfaceRejected = false;
        try
        {
            commands.SetProperty(
                sphere, world_model::kPrimitiveSurface, i64{4});
        }
        catch (const std::exception&)
        {
            surfaceRejected = true;
        }
        check(surfaceRejected);

        // CreatePrimitive authors a preset in one undo step.
        const auto before = world_model::ResolvePrimitives(objects).size();
        auto request = world_model::MakePrimitivePreset(
            world_model::PrimitiveShape::Sphere,
            world_model::PrimitiveSurface::Glass);
        check(request.name == "Glass Sphere");
        request.positionMeters = {1.0, 2.0, 3.0};
        request.sizeMeters = {2.0, 2.0, 2.0};
        const auto glass =
            world_model::CreatePrimitive(commands, body, request);

        resolved = world_model::ResolvePrimitives(objects);
        check(resolved.size() == before + 1U);
        bool foundGlass = false;
        for (const auto& primitive : resolved)
        {
            if (primitive.object == glass)
            {
                foundGlass = true;
                check(primitive.surface ==
                      world_model::PrimitiveSurface::Glass);
                check(primitive.shape == world_model::PrimitiveShape::Sphere);
                check(primitive.positionMeters.z == 3.0);
                check(primitive.sizeMeters.x == 2.0);
                check(primitive.indexOfRefraction == 1.5);
            }
        }
        check(foundGlass);
        check(!commands.HasActiveTransaction());

        const auto mirror = world_model::CreatePrimitive(
            commands,
            body,
            world_model::MakePrimitivePreset(
                world_model::PrimitiveShape::Box,
                world_model::PrimitiveSurface::Mirror));
        static_cast<void>(mirror);
        check(world_model::ResolvePrimitives(objects).size() == before + 2U);

        // Emissive presets carry their luminance.
        auto lamp = world_model::MakePrimitivePreset(
            world_model::PrimitiveShape::Sphere,
            world_model::PrimitiveSurface::Emissive);
        check(lamp.name == "Emissive Sphere");
        check(lamp.emissionNits == 100000.0);
        lamp.emissionNits = 2.5e5;
        const auto lampId =
            world_model::CreatePrimitive(commands, body, lamp);
        bool lampFound = false;
        for (const auto& primitive : world_model::ResolvePrimitives(objects))
        {
            if (primitive.object == lampId)
            {
                lampFound = true;
                check(primitive.surface ==
                      world_model::PrimitiveSurface::Emissive);
                check(primitive.emissionNits == 2.5e5);
            }
        }
        check(lampFound);
        commands.Undo();
        check(world_model::ResolvePrimitives(objects).size() == before + 2U);

        // One undo removes the whole primitive, not just its last property.
        commands.Undo();
        check(world_model::ResolvePrimitives(objects).size() == before + 1U);
    }

    std::filesystem::remove_all(root);
    return failures;
}
