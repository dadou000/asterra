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
bool Near(double a, double b, double rel = 2.0e-6)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}
} // namespace

namespace
{
int FailCode(const int code)
{
    std::cerr << "rotation-composition failure code " << code << '\n';
    return code;
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-rotation-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Rotation Composition Test");
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
            "Locked Moon",
            systemObject);
        const auto orbitCapability = commands.CreateObject(
            orbit::world_model::kOrbitCapabilityType,
            "Orbit",
            bodyObject);
        const auto rotationCapability = commands.CreateObject(
            orbit::world_model::kRotationCapabilityType,
            "Rotation",
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
            rotationCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Synchronous"});
        commands.SetProperty(
            rotationCapability,
            orbit::world_model::kRotationAxis,
            orbit::math::Double3{0.0, 0.0, 1.0});

        orbit::world_model::UniverseComposition composition;
        const auto compositionStats =
            composition.Rebuild(objects);

        if (compositionStats.bodies != 1U)
        {
            return FailCode(1);
        }

        const auto bodyId =
            composition.BodyForObject(bodyObject);
        const auto bodyFrame =
            composition.FrameForObject(bodyObject);
        const auto systemFrame =
            composition.FrameForObject(systemObject);

        if (!bodyId.has_value() ||
            !bodyFrame.has_value() ||
            !systemFrame.has_value())
        {
            return FailCode(1);
        }

        const auto* body =
            composition.Bodies().FindBody(*bodyId);

        if (body == nullptr)
        {
            return FailCode(1);
        }

        const auto centerAtEpoch =
            composition.Frames().ResolveTransform(
                body->centerFrame,
                *systemFrame,
                orbit::time::SimulationTime{});
        const auto atEpoch =
            composition.Frames().ResolveTransform(
                *bodyFrame,
                *systemFrame,
                orbit::time::SimulationTime{});

        if (!centerAtEpoch.has_value() ||
            !atEpoch.has_value() ||
            !Near(centerAtEpoch->translation.x, axis) ||
            !Near(centerAtEpoch->translation.y, 0.0) ||
            !Near(atEpoch->rotation.xAxis.x, -1.0) ||
            !Near(atEpoch->rotation.xAxis.y, 0.0))
        {
            return FailCode(2);
        }

        const double period =
            2.0 * std::numbers::pi_v<double> *
            std::sqrt(axis * axis * axis / mu);

        const orbit::time::SimulationTime quarter{
            .microsecondsFromEpoch =
                static_cast<orbit::i64>(
                    period * 0.25 * 1'000'000.0)
        };

        const auto centerAtQuarter =
            composition.Frames().ResolveTransform(
                body->centerFrame,
                *systemFrame,
                quarter);
        const auto atQuarter =
            composition.Frames().ResolveTransform(
                *bodyFrame,
                *systemFrame,
                quarter);

        if (!centerAtQuarter.has_value() ||
            !atQuarter.has_value() ||
            !Near(centerAtQuarter->translation.x, 0.0) ||
            !Near(centerAtQuarter->translation.y, axis) ||
            !Near(atQuarter->rotation.xAxis.x, 0.0) ||
            !Near(atQuarter->rotation.xAxis.y, -1.0))
        {
            return FailCode(3);
        }

        commands.SetProperty(
            rotationCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Uniform Spin"});
        commands.SetProperty(
            rotationCapability,
            orbit::world_model::kRotationPeriodSeconds,
            4.0);
        commands.SetProperty(
            rotationCapability,
            orbit::world_model::kRotationPhaseDegrees,
            0.0);

        if (!composition.RebuildIfChanged(objects))
        {
            return FailCode(4);
        }

        const auto spun =
            composition.Frames().ResolveTransform(
                *composition.FrameForObject(bodyObject),
                *composition.FrameForObject(systemObject),
                orbit::time::SimulationTime{
                    .microsecondsFromEpoch = 1'000'000});

        if (!spun.has_value() ||
            !Near(spun->rotation.xAxis.x, 0.0) ||
            !Near(spun->rotation.xAxis.y, 1.0))
        {
            return FailCode(5);
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return FailCode(0);
}
