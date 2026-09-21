#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-compact-composition-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Compact Composition Test");

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
                "Shape-Free Compact Body",
                system);

        const auto compact =
            commands.CreateObject(
                kCompactObjectCapabilityType,
                "Compact Object",
                body);

        commands.SetProperty(
            compact,
            kCapabilityModel,
            std::string{
                "Schwarzschild Baseline"});

        commands.SetProperty(
            compact,
            kCompactGravitationalParameter,
            6.4e20);

        bool hasSurface = false;
        bool hasReferenceShape = false;

        for (const auto& child :
             objects.Children(body))
        {
            hasSurface |=
                child.type ==
                kSurfaceCapabilityType;
            hasReferenceShape |=
                child.type ==
                kReferenceShapeCapabilityType;
        }

        if (hasSurface ||
            hasReferenceShape)
        {
            return 1;
        }

        UniverseComposition composition;
        const auto summary =
            composition.Rebuild(objects);

        if (summary.bodies != 1U)
            return 2;

        const auto bodyId =
            composition.BodyForObject(body);

        if (!bodyId.has_value())
            return 3;

        const auto* runtimeBody =
            composition.Bodies().
                FindBody(*bodyId);

        if (runtimeBody == nullptr)
            return 4;

        const auto* envelope =
            std::get_if<
                universe::SphereShape>(
                    &runtimeBody->shape);

        if (envelope == nullptr)
            return 5;

        celestial_compact_objects::
            CompactObjectParameters p{
                .gravitationalParameterM3PerS2 =
                    6.4e20
            };

        const auto expected =
            celestial_compact_objects::
                BuildCompactObjectPresentation(
                    p);

        if (std::abs(
                envelope->radiusMeters -
                expected.shadowRadiusMeters) >
            expected.shadowRadiusMeters *
                1.0e-12)
        {
            return 6;
        }

        commands.SetProperty(
            compact,
            kCompactShadowScale,
            1.2);

        if (!composition.RebuildIfChanged(
                objects))
        {
            return 7;
        }

        const auto* rebuilt =
            composition.Bodies().
                FindBody(*bodyId);

        const auto* rebuiltEnvelope =
            rebuilt != nullptr
                ? std::get_if<
                      universe::SphereShape>(
                          &rebuilt->shape)
                : nullptr;

        if (rebuiltEnvelope == nullptr ||
            rebuiltEnvelope->radiusMeters <=
                envelope->radiusMeters)
        {
            return 8;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
