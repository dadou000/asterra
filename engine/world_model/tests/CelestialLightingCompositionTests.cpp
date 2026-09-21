#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialLightingService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <filesystem>
#include <string>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-celestial-lighting-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Celestial Lighting Test");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);

        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");

        const auto system =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "System",
                worldObject);

        const auto star =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Star",
                system);

        commands.SetProperty(
            star,
            orbit::world_model::kBodyRadius,
            100.0);
        commands.SetProperty(
            star,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{
                0.0, 0.0, 0.0});

        const auto emitter =
            commands.CreateObject(
                orbit::world_model::
                    kRadiativeEmitterCapabilityType,
                "Emitter",
                star);

        commands.SetProperty(
            emitter,
            orbit::world_model::kEmitterDeriveLuminosity,
            false);
        commands.SetProperty(
            emitter,
            orbit::world_model::kEmitterLuminosityWatts,
            1.0e20);

        const auto photosphere =
            commands.CreateObject(
                orbit::world_model::
                    kPhotosphereCapabilityType,
                "Photosphere",
                star);

        commands.SetProperty(
            photosphere,
            orbit::world_model::kPhotosphereRadiusMeters,
            100.0);

        const auto moon =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Moon",
                system);

        commands.SetProperty(
            moon,
            orbit::world_model::kBodyRadius,
            75.0);
        commands.SetProperty(
            moon,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{
                500.0, 0.0, 0.0});

        const auto planet =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Planet",
                system);

        commands.SetProperty(
            planet,
            orbit::world_model::kBodyRadius,
            50.0);
        commands.SetProperty(
            planet,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{
                1000.0, 0.0, 0.0});

        const auto observer =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Observer",
                system);

        commands.SetProperty(
            observer,
            orbit::world_model::kBodyRadius,
            10.0);
        commands.SetProperty(
            observer,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{
                2000.0, 0.0, 0.0});

        orbit::world_model::UniverseComposition universe;
        universe.Rebuild(objects);

        const auto starId =
            universe.BodyForObject(star);
        const auto moonId =
            universe.BodyForObject(moon);
        const auto planetId =
            universe.BodyForObject(planet);
        const auto observerId =
            universe.BodyForObject(observer);

        if (!starId.has_value() ||
            !moonId.has_value() ||
            !planetId.has_value() ||
            !observerId.has_value())
        {
            return 1;
        }

        orbit::world_model::CelestialLightingService
            lighting(
                objects,
                universe);

        const auto clear =
            lighting.DirectLightingAtBody(
                *planetId,
                *starId,
                {},
                {});

        if (!clear.has_value() ||
            clear->visibleFraction != 1.0 ||
            clear->irradianceWattsPerSquareMeter <= 0.0)
        {
            return 2;
        }

        const auto eclipsed =
            lighting.DirectLightingAtBody(
                *planetId,
                *starId,
                {*moonId},
                {});

        if (!eclipsed.has_value() ||
            !(eclipsed->visibleFraction <
                clear->visibleFraction) ||
            !(eclipsed->irradianceWattsPerSquareMeter <
                clear->irradianceWattsPerSquareMeter) ||
            eclipsed->contributingOccluders.size() != 1U)
        {
            return 3;
        }

        const auto reflected =
            lighting.ReflectedLightingAtObserver(
                *observerId,
                *planetId,
                *starId,
                {*moonId},
                {});

        if (!reflected.has_value() ||
            reflected->incidentIrradianceWattsPerSquareMeter <= 0.0 ||
            reflected->unitGeometricAlbedoIrradianceWattsPerSquareMeter < 0.0 ||
            reflected->sourceVisibleFractionAtReflector !=
                eclipsed->visibleFraction)
        {
            return 4;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
