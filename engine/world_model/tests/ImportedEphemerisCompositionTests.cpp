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
#include <stdexcept>
#include <string>
#include <variant>

namespace
{
bool Near(double a, double b, double rel = 1.0e-10)
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
        ("orbit-imported-ephemeris-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    orbit::scene::ObjectId orbitCapabilityId{};
    orbit::scene::ObjectId ephemerisAssetId{};

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Imported Ephemeris Composition Test");
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
            "Imported System",
            worldObject);
        const auto bodyObject = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Imported Body",
            systemObject);

        ephemerisAssetId = commands.CreateObject(
            orbit::world_model::kEphemerisAssetType,
            "JPL-like Test Asset",
            systemObject);

        commands.SetProperty(
            ephemerisAssetId,
            orbit::world_model::kEphemerisSourceLabel,
            std::string{"test-source"});

        const auto sample0 = commands.CreateObject(
            orbit::world_model::kEphemerisSampleType,
            "T0",
            ephemerisAssetId);
        const auto sample1 = commands.CreateObject(
            orbit::world_model::kEphemerisSampleType,
            "T10",
            ephemerisAssetId);

        commands.SetProperty(
            sample0,
            orbit::world_model::kEphemerisSampleTimeMicroseconds,
            orbit::i64{0});
        commands.SetProperty(
            sample0,
            orbit::world_model::kEphemerisSamplePositionMeters,
            orbit::math::Double3{0.0, 0.0, 0.0});
        commands.SetProperty(
            sample0,
            orbit::world_model::kEphemerisSampleVelocityMetersPerSecond,
            orbit::math::Double3{1.0, 0.0, 0.0});

        commands.SetProperty(
            sample1,
            orbit::world_model::kEphemerisSampleTimeMicroseconds,
            orbit::i64{10'000'000});
        commands.SetProperty(
            sample1,
            orbit::world_model::kEphemerisSamplePositionMeters,
            orbit::math::Double3{10.0, 0.0, 0.0});
        commands.SetProperty(
            sample1,
            orbit::world_model::kEphemerisSampleVelocityMetersPerSecond,
            orbit::math::Double3{1.0, 0.0, 0.0});

        orbitCapabilityId = commands.CreateObject(
            orbit::world_model::kOrbitCapabilityType,
            "Imported Orbit",
            bodyObject);

        commands.SetProperty(
            orbitCapabilityId,
            orbit::world_model::kCapabilityModel,
            std::string{"Imported Ephemeris"});
        commands.SetProperty(
            orbitCapabilityId,
            orbit::world_model::kCapabilitySourceObject,
            orbit::schema::ObjectReferenceValue{
                .high = ephemerisAssetId.high,
                .low = ephemerisAssetId.low
            });

        orbit::world_model::UniverseComposition composition;
        composition.Rebuild(objects);

        const auto bodyFrame =
            composition.FrameForObject(bodyObject);
        const auto systemFrame =
            composition.FrameForObject(systemObject);

        if (!bodyFrame.has_value() ||
            !systemFrame.has_value())
        {
            return 1;
        }

        const auto middle =
            composition.Frames().ResolveTransform(
                *bodyFrame,
                *systemFrame,
                orbit::time::SimulationTime{
                    .microsecondsFromEpoch = 5'000'000});

        if (!middle.has_value() ||
            !Near(middle->translation.x, 5.0))
        {
            return 2;
        }

        bool rangeRejected = false;
        try
        {
            static_cast<void>(
                composition.Frames().ResolveTransform(
                    *bodyFrame,
                    *systemFrame,
                    orbit::time::SimulationTime{
                        .microsecondsFromEpoch = 20'000'000}));
        }
        catch (const std::out_of_range& error)
        {
            const std::string message = error.what();
            rangeRejected =
                message.find("20000000") != std::string::npos &&
                message.find("10000000") != std::string::npos &&
                message.find("test-source") != std::string::npos;
        }

        if (!rangeRejected)
        {
            return 3;
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

        const auto source =
            objects.GetProperty(
                orbitCapabilityId,
                orbit::world_model::kCapabilitySourceObject);

        if (!source.has_value() ||
            !std::holds_alternative<
                orbit::schema::ObjectReferenceValue>(*source))
        {
            return 4;
        }

        const auto persisted =
            std::get<
                orbit::schema::ObjectReferenceValue>(*source);

        if (persisted.high != ephemerisAssetId.high ||
            persisted.low != ephemerisAssetId.low)
        {
            return 5;
        }

        const auto asset =
            objects.Find(ephemerisAssetId);

        if (!asset.has_value() ||
            asset->type !=
                orbit::world_model::kEphemerisAssetType)
        {
            return 6;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
