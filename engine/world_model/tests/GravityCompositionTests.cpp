#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
bool Near(double a, double b, double rel = 1.0e-9)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-gravity-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Gravity Composition Test");
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
            "System",
            worldObject);

        const auto bodyObject = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Gravity Body",
            systemObject);

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyMass,
            5.9722e24);

        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{1000.0, 0.0, 0.0});

        const auto gravityCapability = commands.CreateObject(
            orbit::world_model::kGravityCapabilityType,
            "Gravity",
            bodyObject);

        commands.SetProperty(
            gravityCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Point Mass"});
        commands.SetProperty(
            gravityCapability,
            orbit::world_model::kGravityDeriveMuFromMass,
            true);

        orbit::world_model::UniverseComposition composition;
        const auto stats =
            composition.Rebuild(objects);

        if (stats.gravitySources != 1U)
        {
            return 1;
        }

        const auto sourceId =
            composition.GravitySourceForObject(
                bodyObject);
        const auto systemFrame =
            composition.FrameForObject(
                systemObject);

        if (!sourceId.has_value() ||
            !systemFrame.has_value())
        {
            return 2;
        }

        constexpr double radius = 6.371e6;

        const auto acceleration =
            composition.Gravity().AccelerationFrom(
                *sourceId,
                orbit::frames::FramePoint{
                    .frame = *systemFrame,
                    .localMeters = {
                        1000.0 + radius,
                        0.0,
                        0.0
                    }
                },
                orbit::time::SimulationTime{});

        const double expectedMu =
            orbit::celestial_gravity::
                GravitationalParameterFromMass(
                    5.9722e24);
        const double expectedAcceleration =
            expectedMu / (radius * radius);

        if (!acceleration.has_value() ||
            !Near(
                acceleration->x,
                -expectedAcceleration,
                1.0e-8) ||
            !Near(acceleration->y, 0.0) ||
            !Near(acceleration->z, 0.0))
        {
            return 3;
        }

        commands.SetProperty(
            gravityCapability,
            orbit::world_model::kGravityDeriveMuFromMass,
            false);
        commands.SetProperty(
            gravityCapability,
            orbit::world_model::kGravityMuM3PerS2,
            100.0);

        if (!composition.RebuildIfChanged(objects))
        {
            return 4;
        }

        const auto rebuiltSource =
            composition.GravitySourceForObject(
                bodyObject);

        if (!rebuiltSource.has_value() ||
            *rebuiltSource != *sourceId)
        {
            return 5;
        }

        const auto explicitAcceleration =
            composition.Gravity().AccelerationFrom(
                *rebuiltSource,
                orbit::frames::FramePoint{
                    .frame = *composition.FrameForObject(
                        systemObject),
                    .localMeters = {1010.0, 0.0, 0.0}
                },
                orbit::time::SimulationTime{});

        if (!explicitAcceleration.has_value() ||
            !Near(explicitAcceleration->x, -1.0))
        {
            return 6;
        }

        commands.SetProperty(
            gravityCapability,
            orbit::world_model::kCapabilityEnabled,
            false);

        if (!composition.RebuildIfChanged(objects) ||
            composition.GravitySourceForObject(
                bodyObject).has_value())
        {
            return 7;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
