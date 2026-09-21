#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>

int main()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-visibility-proxy-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Visibility Proxy Binding Test");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);

        if (schemas.FindType(
                world_model::
                    kVisibilityProxyType) == nullptr)
        {
            return 1;
        }

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

        const auto group =
            commands.CreateObject(
                world_model::kGeologyAssetType,
                "Future Runtime Object",
                body);

        const auto sphere =
            commands.CreateObject(
                world_model::kVisibilityProxyType,
                "Sphere Proxy",
                group);

        commands.SetProperty(
            sphere,
            world_model::
                kVisibilityProxyPositionMeters,
            math::Double3{1.0, 2.0, 3.0});
        commands.SetProperty(
            sphere,
            world_model::
                kVisibilityProxyRadiusMeters,
            2.5);
        commands.SetProperty(
            sphere,
            world_model::
                kVisibilityProxyMaterialId,
            i64{17});
        commands.SetProperty(
            sphere,
            world_model::
                kVisibilityProxyInstanceId,
            i64{23});

        const auto box =
            commands.CreateObject(
                world_model::kVisibilityProxyType,
                "Box Proxy",
                group);

        commands.SetProperty(
            box,
            world_model::
                kVisibilityProxyShape,
            i64{1});
        commands.SetProperty(
            box,
            world_model::
                kVisibilityProxyHalfExtentsMeters,
            math::Double3{4.0, 5.0, 6.0});
        commands.SetProperty(
            box,
            world_model::
                kVisibilityProxyDynamic,
            true);

        const auto resolved =
            world_model::
                ResolveVisibilityProxies(
                    objects,
                    body);

        if (resolved.size() != 2U ||
            resolved[0].object != sphere ||
            resolved[1].object != box)
        {
            return 2;
        }

        if (resolved[0].radiusMeters != 2.5 ||
            resolved[0].materialId != 17U ||
            resolved[0].instanceId != 23U ||
            resolved[1].shape !=
                world_model::
                    ResolvedVisibilityProxyShape::Box ||
            !resolved[1].dynamic)
        {
            return 3;
        }

        commands.SetProperty(
            sphere,
            world_model::
                kVisibilityProxyEnabled,
            false);

        if (world_model::
                ResolveVisibilityProxies(
                    objects,
                    body).size() != 1U)
        {
            return 4;
        }

        commands.Undo();

        if (world_model::
                ResolveVisibilityProxies(
                    objects,
                    body).size() != 2U)
        {
            return 5;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
