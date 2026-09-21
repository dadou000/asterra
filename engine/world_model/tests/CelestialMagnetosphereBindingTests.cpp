#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>
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
        ("orbit-magnetosphere-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Magnetosphere Binding Test");

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
                "Magnetic World",
                system);

        const auto magnetosphere =
            commands.CreateObject(
                kMagnetosphereCapabilityType,
                "Magnetosphere / Aurora",
                body);

        commands.SetProperty(
            magnetosphere,
            kCapabilityModel,
            std::string{"Parameterized Dipole"});
        commands.SetProperty(
            magnetosphere,
            kMagnetosphereDipoleAxis,
            math::Double3{0.15, 0.05, 0.987});
        commands.SetProperty(
            magnetosphere,
            kMagnetosphereEquatorialFieldTesla,
            4.4e-5);
        commands.SetProperty(
            magnetosphere,
            kMagnetosphereActivity,
            0.72);
        commands.SetProperty(
            magnetosphere,
            kAuroraColorLinear,
            math::Double3{0.2, 2.1, 0.55});
        commands.SetProperty(
            magnetosphere,
            kAuroraStructure,
            0.83);

        constexpr f64 referenceRadius =
            5.2e6;

        const auto resolved =
            ResolveMagnetosphere(
                objects,
                body,
                referenceRadius);

        if (!resolved.has_value() ||
            resolved->fingerprint == 0U ||
            resolved->parameters.equatorialFieldTesla !=
                4.4e-5 ||
            resolved->parameters.activity != 0.72 ||
            resolved->parameters.auroralColorLinear.y != 2.1 ||
            resolved->parameters.auroralStructure != 0.83)
        {
            return 1;
        }

        bool hasAtmosphere = false;
        for (const auto& child :
             objects.Children(body))
        {
            hasAtmosphere |=
                child.type ==
                kAtmosphereCapabilityType;
        }

        if (hasAtmosphere)
        {
            return 2;
        }

        const auto product =
            celestial_magnetosphere::
                BuildMagnetosphereProduct(
                    resolved->parameters,
                    referenceRadius,
                    {.ovalSamples = 64U});

        const auto curtain =
            celestial_magnetosphere::
                BuildAuroraCurtainMesh(
                    resolved->parameters,
                    referenceRadius,
                    64U);

        if (product.northAuroralRing.size() !=
                64U ||
            product.southAuroralRing.size() !=
                64U ||
            curtain.vertices.size() !=
                4U * 65U ||
            curtain.indices.size() !=
                12U * 64U)
        {
            return 3;
        }

        const auto before =
            resolved->fingerprint;

        commands.SetProperty(
            magnetosphere,
            kMagnetosphereActivity,
            0.25);

        const auto revised =
            ResolveMagnetosphere(
                objects,
                body,
                referenceRadius);

        if (!revised.has_value() ||
            revised->fingerprint ==
                before)
        {
            return 4;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
