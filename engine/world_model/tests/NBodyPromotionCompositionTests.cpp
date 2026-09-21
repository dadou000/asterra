#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-nbody-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "N-Body Composition Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);

        const auto worldObject = commands.CreateObject(
            orbit::world_model::kWorldType,
            "World");
        const auto systemObject = commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Dynamic System",
            worldObject);

        const auto star = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Star",
            systemObject);
        const auto planet = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Planet",
            systemObject);

        commands.SetProperty(
            star,
            orbit::world_model::kBodyMass,
            1.98847e30);
        commands.SetProperty(
            star,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{0.0, 0.0, 0.0});

        commands.SetProperty(
            planet,
            orbit::world_model::kBodyMass,
            5.9722e24);

        const auto starOrbit = commands.CreateObject(
            orbit::world_model::kOrbitCapabilityType,
            "Star Orbit",
            star);
        const auto planetOrbit = commands.CreateObject(
            orbit::world_model::kOrbitCapabilityType,
            "Planet Orbit",
            planet);

        commands.SetProperty(
            starOrbit,
            orbit::world_model::kCapabilityModel,
            std::string{"Fixed"});
        commands.SetProperty(
            starOrbit,
            orbit::world_model::kOrbitDynamicPromotionEnabled,
            true);
        commands.SetProperty(
            starOrbit,
            orbit::world_model::kOrbitDynamicStepSeconds,
            3600.0);

        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kCapabilityModel,
            std::string{"Analytic Conic"});
        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitSemiMajorAxisMeters,
            149597870700.0);
        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitEccentricity,
            0.0);
        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitGravitationalParameter,
            1.32712440018e20);
        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitDynamicPromotionEnabled,
            true);
        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitDynamicStepSeconds,
            3600.0);

        orbit::world_model::UniverseComposition composition;
        const auto stats =
            composition.Rebuild(objects);

        if (stats.bodies != 2U)
        {
            return 1;
        }

        const auto starId =
            composition.BodyForObject(star);
        const auto planetId =
            composition.BodyForObject(planet);

        if (!starId.has_value() ||
            !planetId.has_value())
        {
            return 2;
        }

        const auto* starBody =
            composition.Bodies().FindBody(*starId);
        const auto* planetBody =
            composition.Bodies().FindBody(*planetId);

        if (starBody == nullptr ||
            planetBody == nullptr)
        {
            return 3;
        }

        const auto* starTransform =
            std::get_if<
                orbit::universe::ProviderDrivenBodyTransform>(
                    &starBody->transformModel);
        const auto* planetTransform =
            std::get_if<
                orbit::universe::ProviderDrivenBodyTransform>(
                    &planetBody->transformModel);

        if (starTransform == nullptr ||
            planetTransform == nullptr ||
            !starTransform->orbitState ||
            !planetTransform->orbitState ||
            starTransform->orbitState->ModelName() !=
                "Dynamic N-Body" ||
            planetTransform->orbitState->ModelName() !=
                "Dynamic N-Body")
        {
            return 4;
        }

        const auto initial =
            planetTransform->orbitState->
                EvaluateState(
                    orbit::time::SimulationTime{});

        const auto afterDay =
            planetTransform->orbitState->
                EvaluateState(
                    orbit::time::SimulationTime{
                        .microsecondsFromEpoch =
                            86'400'000'000LL
                    });

        if (initial.quality !=
                orbit::celestial_orbits::
                    OrbitStateQuality::DynamicIntegrated ||
            afterDay.quality !=
                orbit::celestial_orbits::
                    OrbitStateQuality::DynamicIntegrated ||
            afterDay.positionMeters.y == 0.0)
        {
            return 5;
        }

        const auto stablePlanetId = *planetId;

        commands.SetProperty(
            planetOrbit,
            orbit::world_model::kOrbitDynamicSofteningMeters,
            1.0);

        // Different sibling settings are intentionally rejected.
        bool mismatchRejected = false;
        try
        {
            static_cast<void>(
                composition.RebuildIfChanged(objects));
        }
        catch (const std::runtime_error&)
        {
            mismatchRejected = true;
        }

        if (!mismatchRejected)
        {
            return 6;
        }

        commands.SetProperty(
            starOrbit,
            orbit::world_model::kOrbitDynamicSofteningMeters,
            1.0);

        if (!composition.RebuildIfChanged(objects))
        {
            return 7;
        }

        if (composition.BodyForObject(planet) !=
            std::optional<orbit::universe::BodyId>(
                stablePlanetId))
        {
            return 8;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
