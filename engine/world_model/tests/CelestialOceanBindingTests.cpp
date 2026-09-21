#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialOceanBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-ocean-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Ocean Binding Test");

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
                "Ocean World",
                system);

        const auto ocean =
            commands.CreateObject(
                kOceanCapabilityType,
                "Ocean",
                body);

        commands.SetProperty(
            ocean,
            kCapabilityModel,
            std::string{
                "Surface Authority"});
        commands.SetProperty(
            ocean,
            kOceanRefractiveIndex,
            1.34);
        commands.SetProperty(
            ocean,
            kOceanOrbitalRoughness,
            0.08);
        commands.SetProperty(
            ocean,
            kOceanDeepWaterColor,
            math::Double3{
                0.01, 0.04, 0.09});
        commands.SetProperty(
            ocean,
            kOceanGlintStrength,
            1.7);

        const auto resolved =
            ResolveOceanBody(
                objects,
                body);

        if (!resolved.has_value() ||
            resolved->oceanCapability !=
                ocean ||
            resolved->optical.refractiveIndex !=
                1.34 ||
            resolved->optical.orbitalRoughness !=
                0.08 ||
            resolved->optical.glintStrength !=
                1.7 ||
            resolved->optical.deepWaterColor.z !=
                0.09)
        {
            return 1;
        }

        const auto fingerprint =
            celestial_ocean::
                OceanOpticalFingerprint(
                    resolved->optical);

        commands.SetProperty(
            ocean,
            kOceanOrbitalRoughness,
            0.25);

        const auto revised =
            ResolveOceanBody(
                objects,
                body);

        if (!revised.has_value() ||
            celestial_ocean::
                OceanOpticalFingerprint(
                    revised->optical) ==
                fingerprint)
        {
            return 2;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
