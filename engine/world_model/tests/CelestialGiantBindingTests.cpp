#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
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
        ("orbit-giant-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Giant Binding Test");

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
                "Ice Giant",
                system);

        commands.SetProperty(
            body,
            kBodyRadius,
            2.5e7);

        const auto giant =
            commands.CreateObject(
                kGiantAppearanceCapabilityType,
                "Giant Appearance",
                body);

        commands.SetProperty(
            giant,
            kCapabilityModel,
            std::string{"Procedural Giant"});
        commands.SetProperty(
            giant,
            kGiantClass,
            std::string{"Ice Giant"});
        commands.SetProperty(
            giant,
            kGiantSeed,
            i64{42});

        const auto resolved =
            ResolveGiantAppearance(
                objects,
                body);

        if (!resolved.has_value() ||
            resolved->parameters.giantClass !=
                celestial_giants::GiantClass::IceGiant ||
            resolved->parameters.seed != 42U ||
            resolved->parameters.baseColorLinear.z <=
                resolved->parameters.baseColorLinear.x ||
            resolved->fingerprint == 0U)
        {
            return 1;
        }

        bool hasSurface = false;
        for (const auto& child :
             objects.Children(body))
        {
            hasSurface |=
                child.type ==
                kSurfaceCapabilityType;
        }

        if (hasSurface)
        {
            return 2;
        }

        const auto appearance =
            celestial_giants::
                BuildGiantAppearance(
                    resolved->parameters,
                    {.faceResolution = 17U});

        if (appearance.texels.size() !=
                6U * 17U * 17U ||
            appearance.fingerprint == 0U)
        {
            return 3;
        }

        bool varied = false;
        const auto first =
            appearance.texels.front().
                albedoLinear;

        for (const auto& texel :
             appearance.texels)
        {
            if (texel.albedoLinear.x !=
                    first.x ||
                texel.albedoLinear.y !=
                    first.y ||
                texel.albedoLinear.z !=
                    first.z)
            {
                varied = true;
                break;
            }
        }

        if (!varied)
        {
            return 4;
        }

        const auto before =
            resolved->fingerprint;

        commands.SetProperty(
            giant,
            kGiantBandStrength,
            0.65);

        const auto revised =
            ResolveGiantAppearance(
                objects,
                body);

        if (!revised.has_value() ||
            revised->fingerprint ==
                before)
        {
            return 5;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
