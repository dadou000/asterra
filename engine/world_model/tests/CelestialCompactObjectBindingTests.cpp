#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialCompactObjectBinding.hpp>
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
        ("orbit-compact-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Compact Object Binding Test");

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
                "Black Hole",
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
            5.0e20);
        commands.SetProperty(
            compact,
            kCompactPhotonRingIntensity,
            1.35);

        const auto flow =
            commands.CreateObject(
                kAccretionFlowCapabilityType,
                "Accretion Flow",
                body);

        commands.SetProperty(
            flow,
            kCapabilityModel,
            std::string{
                "Thin Flow Baseline"});
        commands.SetProperty(
            flow,
            kAccretionInnerRadiusRg,
            6.0);
        commands.SetProperty(
            flow,
            kAccretionOuterRadiusRg,
            55.0);
        commands.SetProperty(
            flow,
            kAccretionIntensity,
            2.0);

        bool hasSurface = false;
        bool hasShape = false;

        for (const auto& child :
             objects.Children(body))
        {
            hasSurface |=
                child.type ==
                kSurfaceCapabilityType;
            hasShape |=
                child.type ==
                kReferenceShapeCapabilityType;
        }

        if (hasSurface || hasShape)
            return 1;

        const auto resolved =
            ResolveCompactObject(
                objects,
                body);

        if (!resolved.has_value() ||
            resolved->fingerprint == 0U ||
            resolved->parameters.
                    gravitationalParameterM3PerS2 !=
                5.0e20 ||
            resolved->parameters.
                    photonRingIntensity !=
                1.35)
        {
            return 2;
        }

        const auto resolvedFlow =
            ResolveAccretionFlow(
                objects,
                body,
                resolved->parameters);

        if (!resolvedFlow.has_value() ||
            resolvedFlow->fingerprint == 0U ||
            resolvedFlow->parameters.
                    outerRadiusRg !=
                55.0)
        {
            return 3;
        }

        const auto presentation =
            celestial_compact_objects::
                BuildCompactObjectPresentation(
                    resolved->parameters);

        const auto accretion =
            celestial_compact_objects::
                BuildAccretionFlowProduct(
                    resolvedFlow->parameters,
                    resolved->parameters,
                    32U);

        if (!(presentation.shadowRadiusMeters >
              presentation.scales.
                  schwarzschildRadiusMeters) ||
            accretion.radialProfile.size() !=
                32U)
        {
            return 4;
        }

        const auto before =
            resolved->fingerprint;

        commands.SetProperty(
            compact,
            kCompactShadowScale,
            1.18);

        const auto revised =
            ResolveCompactObject(
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
