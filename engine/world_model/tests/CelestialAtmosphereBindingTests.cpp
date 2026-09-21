#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-atmosphere-binding-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Atmosphere Binding Test");

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

        const auto body =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Planet",
                system);

        constexpr orbit::f64 radius =
            7.0e6;

        commands.SetProperty(
            body,
            orbit::world_model::kBodyRadius,
            radius);

        const auto atmosphere =
            commands.CreateObject(
                orbit::world_model::kAtmosphereCapabilityType,
                "Atmosphere",
                body);

        commands.SetProperty(
            atmosphere,
            orbit::world_model::kCapabilityModel,
            std::string{"Physical Scattering"});

        commands.SetProperty(
            atmosphere,
            orbit::world_model::kAtmosphereTopRadiusMeters,
            radius + 120000.0);

        commands.SetProperty(
            atmosphere,
            orbit::world_model::kAtmosphereRayleighScaleHeightMeters,
            9100.0);

        const auto resolved =
            orbit::world_model::ResolveAtmosphereBody(
                objects,
                body);

        if (!resolved.has_value() ||
            resolved->parameters.bottomRadiusMeters !=
                radius ||
            resolved->parameters.topRadiusMeters !=
                radius + 120000.0 ||
            resolved->parameters.rayleighScaleHeightMeters !=
                9100.0)
        {
            return 1;
        }

        const auto fingerprint =
            orbit::celestial_atmosphere::AtmosphereFingerprint(
                resolved->parameters);

        commands.SetProperty(
            atmosphere,
            orbit::world_model::kAtmosphereRayleighScaleHeightMeters,
            9200.0);

        const auto revised =
            orbit::world_model::ResolveAtmosphereBody(
                objects,
                body);

        if (!revised.has_value() ||
            orbit::celestial_atmosphere::AtmosphereFingerprint(
                revised->parameters) ==
                fingerprint)
        {
            return 2;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
