#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>
#include <variant>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-celestial-capability-schema-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    orbit::scene::ObjectId capabilityId{};

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Celestial Capability Schema Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);

        const auto* atmosphereSchema =
            schemas.FindType(
                orbit::world_model::kAtmosphereCapabilityType);
        const auto* orbitSchema =
            schemas.FindType(
                orbit::world_model::kOrbitCapabilityType);

        if (atmosphereSchema == nullptr ||
            orbitSchema == nullptr ||
            atmosphereSchema->category != "Celestial/Capability")
        {
            return 1;
        }

        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(objects, schemas);

        const auto worldObject = commands.CreateObject(
            orbit::world_model::kWorldType, "World");
        const auto systemObject = commands.CreateObject(
            orbit::world_model::kCelestialSystemType, "Helion", worldObject);
        const auto bodyObject = commands.CreateObject(
            orbit::world_model::kCelestialBodyType, "Asterra", systemObject);

        capabilityId = commands.CreateObject(
            orbit::world_model::kAtmosphereCapabilityType,
            "Atmosphere",
            bodyObject);

        commands.SetProperty(
            capabilityId,
            orbit::world_model::kCapabilityModel,
            std::string{"Physical Multiscattering"});

        commands.SetProperty(
            capabilityId,
            orbit::world_model::kCapabilityEnabled,
            false);

        commands.Undo();
        if (objects.GetProperty(
                capabilityId,
                orbit::world_model::kCapabilityEnabled).has_value())
        {
            return 2;
        }

        commands.Redo();

        const auto enabled = objects.GetProperty(
            capabilityId,
            orbit::world_model::kCapabilityEnabled);
        if (!enabled.has_value() ||
            !std::holds_alternative<bool>(*enabled) ||
            std::get<bool>(*enabled))
        {
            return 3;
        }

        world.Checkpoint();
    }

    {
        orbit::documents::ProjectDocument project(root);
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath(),
            orbit::documents::WorldOpenMode::ReadOnly);
        orbit::scene::ObjectStore objects(world);

        const auto persisted = objects.Find(capabilityId);
        if (!persisted.has_value() ||
            persisted->type !=
                orbit::world_model::kAtmosphereCapabilityType ||
            persisted->name != "Atmosphere")
        {
            return 4;
        }

        const auto model = objects.GetProperty(
            capabilityId,
            orbit::world_model::kCapabilityModel);

        if (!model.has_value() ||
            !std::holds_alternative<std::string>(*model) ||
            std::get<std::string>(*model) !=
                "Physical Multiscattering")
        {
            return 5;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
