#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialSmallBodyBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-small-body-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Small Body Binding Test");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        RegisterSchemas(schemas);

        scene::ObjectStore objects(world);
        commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                kWorldType,
                "World");

        const auto system =
            commands.CreateObject(
                kCelestialSystemType,
                "System",
                worldObject);

        const auto body =
            commands.CreateObject(
                kCelestialBodyType,
                "Irregular Asteroid",
                system);

        commands.SetProperty(
            body,
            kBodyRadius,
            8500.0);

        const auto smallBody =
            commands.CreateObject(
                kSmallBodyAppearanceCapabilityType,
                "Small Body Appearance",
                body);

        commands.SetProperty(
            smallBody,
            kCapabilityModel,
            std::string{"Procedural Regolith"});
        commands.SetProperty(
            smallBody,
            kSmallBodyClass,
            std::string{"Asteroid"});
        commands.SetProperty(
            smallBody,
            kSmallBodySeed,
            i64{73});
        commands.SetProperty(
            smallBody,
            kSmallBodyAxisScale,
            math::Double3{1.65, 0.78, 0.56});
        commands.SetProperty(
            smallBody,
            kSmallBodyIrregularity,
            0.27);
        commands.SetProperty(
            smallBody,
            kSmallBodyCraterDensity,
            0.72);

        const auto resolved =
            ResolveSmallBodyAppearance(
                objects,
                body);

        if (!resolved.has_value() ||
            resolved->parameters.bodyClass !=
                celestial_small_bodies::
                    SmallBodyClass::Asteroid ||
            resolved->parameters.seed != 73U ||
            resolved->parameters.axisScale.x != 1.65 ||
            resolved->fingerprint == 0U)
        {
            return 1;
        }

        bool hasSurface = false;
        for (const auto& child :
             objects.Children(body))
        {
            hasSurface |=
                child.type ==
                kSurfaceCapabilityType;
        }

        if (hasSurface)
        {
            return 2;
        }

        const auto shape =
            celestial_small_bodies::
                BuildSmallBodyShape(
                    resolved->parameters,
                    {.faceResolution = 17U});

        const auto appearance =
            celestial_small_bodies::
                BuildSmallBodyAppearance(
                    resolved->parameters,
                    {.faceResolution = 17U});

        if (!(shape.maximumRadiusScale >
              shape.minimumRadiusScale) ||
            shape.radiusScale.size() !=
                6U * 17U * 17U ||
            appearance.texels.size() !=
                6U * 17U * 17U)
        {
            return 3;
        }

        const auto before =
            resolved->fingerprint;

        commands.SetProperty(
            smallBody,
            kSmallBodyCraterDensity,
            0.35);

        const auto revised =
            ResolveSmallBodyAppearance(
                objects,
                body);

        if (!revised.has_value() ||
            revised->fingerprint ==
                before)
        {
            return 4;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
