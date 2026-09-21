#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <optional>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-reference-node-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Reference Node Composition Test");
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
            "Binary Test",
            worldObject);

        const auto binaryBarycenter = commands.CreateObject(
            orbit::world_model::kCelestialReferenceNodeType,
            "AB Barycenter",
            systemObject);

        commands.SetProperty(
            binaryBarycenter,
            orbit::world_model::kReferenceNodePositionMeters,
            orbit::math::Double3{1'000.0, 2'000.0, 3'000.0});

        const auto starA = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Star A",
            binaryBarycenter);
        const auto nestedBarycenter = commands.CreateObject(
            orbit::world_model::kCelestialReferenceNodeType,
            "Planet-Moon Barycenter",
            binaryBarycenter);
        const auto planet = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Planet",
            nestedBarycenter);
        const auto moon = commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Moon",
            nestedBarycenter);

        commands.SetProperty(
            starA,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{-10'000.0, 0.0, 0.0});
        commands.SetProperty(
            nestedBarycenter,
            orbit::world_model::kReferenceNodePositionMeters,
            orbit::math::Double3{20'000.0, 0.0, 0.0});
        commands.SetProperty(
            planet,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{-500.0, 0.0, 0.0});
        commands.SetProperty(
            moon,
            orbit::world_model::kBodyParentPositionMeters,
            orbit::math::Double3{5'000.0, 0.0, 0.0});

        orbit::world_model::UniverseComposition composition;
        const auto first = composition.Rebuild(objects);

        if (first.systems != 1U ||
            first.referenceNodes != 2U ||
            first.bodies != 3U)
        {
            return 1;
        }

        const auto systemFrame =
            composition.FrameForObject(systemObject);
        const auto binaryFrame =
            composition.FrameForObject(binaryBarycenter);
        const auto nestedFrame =
            composition.FrameForObject(nestedBarycenter);
        const auto starFrame =
            composition.FrameForObject(starA);
        const auto planetFrame =
            composition.FrameForObject(planet);
        const auto moonFrame =
            composition.FrameForObject(moon);

        if (!systemFrame.has_value() ||
            !binaryFrame.has_value() ||
            !nestedFrame.has_value() ||
            !starFrame.has_value() ||
            !planetFrame.has_value() ||
            !moonFrame.has_value())
        {
            return 2;
        }

        if (composition.BodyForObject(binaryBarycenter).has_value() ||
            composition.BodyForObject(nestedBarycenter).has_value())
        {
            return 3;
        }

        const auto starId =
            composition.BodyForObject(starA);
        const auto planetId =
            composition.BodyForObject(planet);
        const auto moonId =
            composition.BodyForObject(moon);

        const auto* starBody =
            starId.has_value()
                ? composition.Bodies().FindBody(*starId)
                : nullptr;
        const auto* planetBody =
            planetId.has_value()
                ? composition.Bodies().FindBody(*planetId)
                : nullptr;
        const auto* moonBody =
            moonId.has_value()
                ? composition.Bodies().FindBody(*moonId)
                : nullptr;

        if (starBody == nullptr ||
            planetBody == nullptr ||
            moonBody == nullptr)
        {
            return 4;
        }

        if (composition.Frames().Parent(*binaryFrame) != systemFrame ||
            composition.Frames().Parent(*nestedFrame) != binaryFrame ||
            composition.Frames().Parent(starBody->centerFrame) != binaryFrame ||
            composition.Frames().Parent(planetBody->centerFrame) != nestedFrame ||
            composition.Frames().Parent(moonBody->centerFrame) != nestedFrame ||
            composition.Frames().Parent(*starFrame) != starBody->centerFrame ||
            composition.Frames().Parent(*planetFrame) != planetBody->centerFrame ||
            composition.Frames().Parent(*moonFrame) != moonBody->centerFrame)
        {
            return 5;
        }

        const auto binaryToSystem =
            composition.Frames().ResolveTransform(
                *binaryFrame,
                *systemFrame,
                orbit::time::SimulationTime{});

        if (!binaryToSystem.has_value() ||
            binaryToSystem->translation.x != 1'000.0 ||
            binaryToSystem->translation.y != 2'000.0 ||
            binaryToSystem->translation.z != 3'000.0)
        {
            return 9;
        }

        const auto stableNestedFrame = *nestedFrame;

        commands.SetProperty(
            nestedBarycenter,
            orbit::world_model::kReferenceNodePositionMeters,
            orbit::math::Double3{25'000.0, 0.0, 0.0});

        if (!composition.RebuildIfChanged(objects))
        {
            return 6;
        }

        const auto rebuiltNested =
            composition.FrameForObject(nestedBarycenter);

        if (!rebuiltNested.has_value() ||
            *rebuiltNested != stableNestedFrame)
        {
            return 7;
        }

        const auto nestedToBinary =
            composition.Frames().ResolveTransform(
                *rebuiltNested,
                *composition.FrameForObject(binaryBarycenter),
                orbit::time::SimulationTime{});

        if (!nestedToBinary.has_value() ||
            nestedToBinary->translation.x != 25'000.0)
        {
            return 8;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
