#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/LocalLightBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-local-light-binding-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    orbit::scene::ObjectId bodyObject{};
    orbit::scene::ObjectId pointLight{};
    orbit::scene::ObjectId spotLight{};

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Local Light Binding Test");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);

        if (schemas.FindType(
                orbit::world_model::kPointLightType) == nullptr ||
            schemas.FindType(
                orbit::world_model::kSpotLightType) == nullptr)
        {
            return 1;
        }

        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");

        const auto systemObject =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldObject);

        bodyObject =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Asterra",
                systemObject);

        pointLight =
            commands.CreateObject(
                orbit::world_model::kPointLightType,
                "Lamp",
                bodyObject);

        commands.SetProperty(
            pointLight,
            orbit::world_model::kLightPositionMeters,
            orbit::math::Double3{1.0, 2.0, 3.0});
        commands.SetProperty(
            pointLight,
            orbit::world_model::kLightIntensityLumens,
            1600.0);
        commands.SetProperty(
            pointLight,
            orbit::world_model::kLightRangeMeters,
            18.0);

        spotLight =
            commands.CreateObject(
                orbit::world_model::kSpotLightType,
                "Headlight",
                bodyObject);

        commands.SetProperty(
            spotLight,
            orbit::world_model::kLightPositionMeters,
            orbit::math::Double3{4.0, 5.0, 6.0});
        commands.SetProperty(
            spotLight,
            orbit::world_model::kLightDirection,
            orbit::math::Double3{0.0, -0.2, 1.0});
        commands.SetProperty(
            spotLight,
            orbit::world_model::kLightIntensityLumens,
            2400.0);
        commands.SetProperty(
            spotLight,
            orbit::world_model::kLightInnerConeDegrees,
            15.0);
        commands.SetProperty(
            spotLight,
            orbit::world_model::kLightOuterConeDegrees,
            28.0);

        const auto resolved =
            orbit::world_model::
                ResolveAuthoredLocalLights(
                    objects,
                    bodyObject);

        if (resolved.size() != 2U ||
            resolved[0].object != pointLight ||
            resolved[1].object != spotLight)
        {
            return 2;
        }

        if (resolved[0].luminousFluxLumens != 1600.0 ||
            resolved[0].rangeMeters != 18.0 ||
            resolved[1].innerConeDegrees != 15.0 ||
            resolved[1].outerConeDegrees != 28.0)
        {
            return 3;
        }

        commands.SetProperty(
            pointLight,
            orbit::world_model::kLightEnabled,
            false);

        const auto disabled =
            orbit::world_model::
                ResolveAuthoredLocalLights(
                    objects,
                    bodyObject);

        if (disabled.size() != 1U ||
            disabled.front().object != spotLight)
        {
            return 4;
        }

        commands.Undo();

        const auto restored =
            orbit::world_model::
                ResolveAuthoredLocalLights(
                    objects,
                    bodyObject);

        if (restored.size() != 2U)
        {
            return 5;
        }

        world.Checkpoint();
    }

    {
        auto project =
            orbit::documents::ProjectDocument::Open(
                root / "Project.orbit.toml");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath(),
            orbit::documents::WorldOpenMode::ReadOnly);

        orbit::scene::ObjectStore objects(world);

        const auto restored =
            orbit::world_model::
                ResolveAuthoredLocalLights(
                    objects,
                    bodyObject);

        if (restored.size() != 2U ||
            restored[0].object != pointLight ||
            restored[1].object != spotLight)
        {
            return 6;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
