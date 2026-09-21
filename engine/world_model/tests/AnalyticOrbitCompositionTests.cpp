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
#include <numbers>
#include <string>
#include <iostream>

namespace
{
bool Near(
    const double a,
    const double b,
    const double relative = 2.0e-6)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <=
        relative * scale;
}
} // namespace

namespace
{
int FailCode(const int code)
{
    std::cerr << "analytic-orbit failure code " << code << '\\n';
    return code;
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-analytic-orbit-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Analytic Orbit Composition Test");
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
            "Orbiter",
            systemObject);
        const auto orbitCapability = commands.CreateObject(
            orbit::world_model::kOrbitCapabilityType,
            "Orbit",
            bodyObject);

        constexpr double axis = 7.0e6;
        constexpr double mu = 3.986004418e14;

        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Analytic Conic"});
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitSemiMajorAxisMeters,
            axis);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitEccentricity,
            0.0);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitGravitationalParameter,
            mu);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitEpochMicroseconds,
            orbit::i64{0});

        orbit::world_model::UniverseComposition composition;
        const auto stats =
            composition.Rebuild(objects);

        if (stats.bodies != 1U)
        {
            return FailCode(1);
        }

        const auto bodyFrame =
            composition.FrameForObject(bodyObject);
        const auto systemFrame =
            composition.FrameForObject(systemObject);

        if (!bodyFrame.has_value() ||
            !systemFrame.has_value())
        {
            return FailCode(2);
        }

        const auto atEpoch =
            composition.Frames().ResolveTransform(
                *bodyFrame,
                *systemFrame,
                orbit::time::SimulationTime{});

        if (!atEpoch.has_value() ||
            !Near(atEpoch->translation.x, axis) ||
            !Near(atEpoch->translation.y, 0.0))
        {
            return FailCode(3);
        }

        const double period =
            2.0 * std::numbers::pi_v<double> *
            std::sqrt(
                axis * axis * axis / mu);

        const orbit::time::SimulationTime quarter{
            .microsecondsFromEpoch =
                static_cast<orbit::i64>(
                    period * 0.25 * 1'000'000.0)
        };

        const auto atQuarter =
            composition.Frames().ResolveTransform(
                *bodyFrame,
                *systemFrame,
                quarter);

        if (!atQuarter.has_value() ||
            !Near(atQuarter->translation.x, 0.0) ||
            !Near(atQuarter->translation.y, axis))
        {
            return FailCode(4);
        }

        // A disabled orbit capability must fall back to the legacy fixed
        // parent-position authority through the same OrbitStateProvider seam.
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kCapabilityEnabled,
            false);
        commands.SetProperty(
            bodyObject,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{11.0, 22.0, 33.0});

        if (!composition.RebuildIfChanged(objects))
        {
            return FailCode(5);
        }

        const auto fixedBodyFrame =
            composition.FrameForObject(bodyObject);
        const auto fixedSystemFrame =
            composition.FrameForObject(systemObject);
        const auto fixed =
            composition.Frames().ResolveTransform(
                *fixedBodyFrame,
                *fixedSystemFrame,
                quarter);

        if (!fixed.has_value() ||
            fixed->translation.x != 11.0 ||
            fixed->translation.y != 22.0 ||
            fixed->translation.z != 33.0)
        {
            return FailCode(6);
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return FailCode(0);
}
