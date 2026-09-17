#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
#include <variant>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-universe-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    auto project =
        orbit::documents::ProjectDocument::Create(
            root,
            "Universe Composition Test");
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
    const auto systemObject =
        commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
    const auto planetObject =
        commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Asterra",
            systemObject);
    const auto moonObject =
        commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Luma",
            planetObject);

    commands.SetProperty(
        systemObject,
        orbit::world_model::kSystemEpochMicroseconds,
        orbit::i64{123456});

    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyEllipsoidEnabled,
        true);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyRadius,
        6'400'000.0);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyPolarRadius,
        6'350'000.0);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyMass,
        5.5e24);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyParentPositionMeters,
        orbit::math::Double3{
            10'000.0,
            20'000.0,
            30'000.0
        });
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyRotationPeriodSeconds,
        80'000.0);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyAxialTiltDegrees,
        24.0);
    commands.SetProperty(
        planetObject,
        orbit::world_model::kBodyRotationPhaseDegrees,
        15.0);

    commands.SetProperty(
        moonObject,
        orbit::world_model::kBodyRadius,
        1'700'000.0);
    commands.SetProperty(
        moonObject,
        orbit::world_model::kBodyMass,
        7.0e22);
    commands.SetProperty(
        moonObject,
        orbit::world_model::kBodyParentPositionMeters,
        orbit::math::Double3{
            400'000'000.0,
            0.0,
            0.0
        });

    orbit::world_model::UniverseComposition composition;
    const auto first =
        composition.Rebuild(objects);

    if (first.systems != 1U ||
        first.bodies != 2U ||
        first.sourceRevision != objects.Revision())
    {
        return 1;
    }

    const auto systemId =
        composition.SystemForObject(systemObject);
    const auto planetId =
        composition.BodyForObject(planetObject);
    const auto moonId =
        composition.BodyForObject(moonObject);

    if (!systemId.has_value() ||
        !planetId.has_value() ||
        !moonId.has_value())
    {
        return 2;
    }

    const auto* system =
        composition.Bodies().FindSystem(*systemId);
    const auto* planet =
        composition.Bodies().FindBody(*planetId);
    const auto* moon =
        composition.Bodies().FindBody(*moonId);

    if (system == nullptr ||
        planet == nullptr ||
        moon == nullptr)
    {
        return 3;
    }

    const auto* ellipsoid =
        std::get_if<orbit::universe::EllipsoidShape>(
            &planet->shape);

    if (ellipsoid == nullptr ||
        ellipsoid->radiiMeters.x != 6'400'000.0 ||
        ellipsoid->radiiMeters.y != 6'400'000.0 ||
        ellipsoid->radiiMeters.z != 6'350'000.0)
    {
        return 4;
    }

    const auto* transform =
        std::get_if<orbit::universe::UniformRotationTransform>(
            &planet->transformModel);

    if (transform == nullptr ||
        transform->centerInParentMeters.x != 10'000.0 ||
        transform->centerInParentMeters.y != 20'000.0 ||
        transform->centerInParentMeters.z != 30'000.0 ||
        transform->epoch.microsecondsFromEpoch != 123456 ||
        transform->angularVelocityRadiansPerSecond <= 0.0)
    {
        return 5;
    }

    if (moon->parentFrame != planet->frame ||
        composition.ObjectForBody(*moonId) !=
            std::optional<orbit::scene::ObjectId>(moonObject))
    {
        return 6;
    }

    if (composition.RebuildIfChanged(objects))
    {
        return 7;
    }

    commands.SetProperty(
        moonObject,
        orbit::world_model::kBodyRadius,
        1'800'000.0);

    if (!composition.RebuildIfChanged(objects))
    {
        return 8;
    }

    const auto stableMoonId =
        composition.BodyForObject(moonObject);

    if (!stableMoonId.has_value() ||
        *stableMoonId != *moonId)
    {
        return 9;
    }

    const auto* rebuiltMoon =
        composition.Bodies().FindBody(*stableMoonId);
    const auto* rebuiltSphere =
        rebuiltMoon != nullptr
            ? std::get_if<orbit::universe::SphereShape>(
                  &rebuiltMoon->shape)
            : nullptr;

    if (rebuiltSphere == nullptr ||
        rebuiltSphere->radiusMeters != 1'800'000.0)
    {
        return 10;
    }

    world.Checkpoint();
    std::filesystem::remove_all(root);
    return 0;
}
