#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialCloudBinding.hpp>
#include <orbit/world_model/CelestialLightingService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-cloud-composition-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Cloud Composition Test");

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

        const auto star =
            commands.CreateObject(
                kCelestialBodyType,
                "Star",
                system);

        commands.SetProperty(
            star,
            kBodyRadius,
            100.0);
        commands.SetProperty(
            star,
            kBodyParentPositionMeters,
            math::Double3{0.0, 0.0, 0.0});

        const auto emitter =
            commands.CreateObject(
                kRadiativeEmitterCapabilityType,
                "Emitter",
                star);

        commands.SetProperty(
            emitter,
            kEmitterDeriveLuminosity,
            false);
        commands.SetProperty(
            emitter,
            kEmitterLuminosityWatts,
            1.0e20);

        const auto photosphere =
            commands.CreateObject(
                kPhotosphereCapabilityType,
                "Photosphere",
                star);

        commands.SetProperty(
            photosphere,
            kPhotosphereRadiusMeters,
            100.0);

        const auto planet =
            commands.CreateObject(
                kCelestialBodyType,
                "Planet",
                system);

        commands.SetProperty(
            planet,
            kBodyRadius,
            50.0);
        commands.SetProperty(
            planet,
            kBodyParentPositionMeters,
            math::Double3{1000.0, 0.0, 0.0});

        const auto cloud =
            commands.CreateObject(
                kCloudLayerCapabilityType,
                "Cloud Layer",
                planet);

        commands.SetProperty(
            cloud,
            kCapabilityModel,
            std::string{"Volumetric Layer"});
        commands.SetProperty(
            cloud,
            kCloudSourceModel,
            std::string{"Procedural"});
        commands.SetProperty(
            cloud,
            kCloudCoverageBias,
            1.0);
        commands.SetProperty(
            cloud,
            kCloudOpticalDepth,
            4.0);
        commands.SetProperty(
            cloud,
            kCloudSeed,
            i64{123});

        const auto layers =
            ResolveCloudLayers(
                objects,
                planet);

        if (layers.size() != 1U ||
            layers.front().
                parameters.sourceModel !=
                celestial_clouds::
                    CloudSourceModel::
                        Procedural ||
            layers.front().
                parameters.peakOpticalDepth !=
                4.0)
        {
            return 1;
        }

        const auto field =
            celestial_clouds::
                BuildCloudField(
                    nullptr,
                    nullptr,
                    50.0,
                    {layers.front().parameters},
                    {},
                    {
                        .faceResolution = 9U,
                        .footprintScale = 2.0,
                        .timeQuantumMicroseconds =
                            1'000'000
                    });

        if (field.layers.size() != 1U ||
            field.fingerprint == 0U)
        {
            return 2;
        }

        celestial_appearance::
            PlanetaryAppearanceProduct
            appearance;

        appearance.faceResolution = 3U;
        appearance.fingerprint = 55U;
        appearance.texels.resize(
            6U * 3U * 3U);

        for (auto& texel :
             appearance.texels)
        {
            texel.albedoLinear = {
                0.1F, 0.15F, 0.2F};
            texel.roughness = 0.5F;
        }

        const u64 appearanceBefore =
            appearance.fingerprint;

        celestial_clouds::
            CompositeOrbitalCloudAppearance(
                field,
                appearance);

        if (appearance.fingerprint ==
                appearanceBefore ||
            appearance.texels.front().
                albedoLinear.x <=
                0.1F)
        {
            return 3;
        }

        UniverseComposition universe;
        universe.Rebuild(objects);

        const auto starId =
            universe.BodyForObject(
                star);
        const auto planetId =
            universe.BodyForObject(
                planet);

        if (!starId.has_value() ||
            !planetId.has_value())
        {
            return 4;
        }

        CelestialLightingService lighting(
            objects,
            universe);

        const auto clear =
            lighting.DirectLightingAtBody(
                *planetId,
                *starId,
                {},
                {});

        const auto cloudy =
            lighting.DirectLightingAtSurface(
                *planetId,
                *starId,
                {},
                {},
                field,
                {-1.0, 0.0, 0.0});

        if (!clear.has_value() ||
            !cloudy.has_value() ||
            !(cloudy->cloudTransmittance <
                1.0) ||
            !(cloudy->
                  irradianceWattsPerSquareMeter <
              clear->
                  irradianceWattsPerSquareMeter) ||
            cloudy->celestial.visibleFraction !=
                clear->visibleFraction)
        {
            return 5;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
