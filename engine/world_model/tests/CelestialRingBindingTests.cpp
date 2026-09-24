#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialLightingService.hpp>
#include <orbit/world_model/CelestialRingBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-ring-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Ring Binding Test");

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

        const auto star =
            commands.CreateObject(
                kCelestialBodyType,
                "Star",
                system);

        commands.SetProperty(
            star,
            kBodyRadius,
            10.0);
        // Body-fixed +Y is the spin pole, which maps to the parent +Z pole
        // for an untilted body. Placing the star slightly above the parent
        // XY (equatorial) plane puts it 0.1 rad above the equatorial ring
        // plane in body coordinates.
        commands.SetProperty(
            star,
            kBodyParentPositionMeters,
            math::Double3{
                1000.0, 0.0, 100.0});

        const auto emitter =
            commands.CreateObject(
                kRadiativeEmitterCapabilityType,
                "Emitter",
                star);
        commands.SetProperty(
            emitter,
            kEmitterDeriveLuminosity,
            false);
        commands.SetProperty(
            emitter,
            kEmitterLuminosityWatts,
            1.0e12);

        const auto photosphere =
            commands.CreateObject(
                kPhotosphereCapabilityType,
                "Photosphere",
                star);
        commands.SetProperty(
            photosphere,
            kPhotosphereRadiusMeters,
            10.0);

        const auto planet =
            commands.CreateObject(
                kCelestialBodyType,
                "Ringed Planet",
                system);

        commands.SetProperty(
            planet,
            kBodyRadius,
            1.0);
        commands.SetProperty(
            planet,
            kBodyParentPositionMeters,
            math::Double3{});

        const auto ringSystem =
            commands.CreateObject(
                kRingSystemCapabilityType,
                "Rings",
                planet);

        commands.SetProperty(
            ringSystem,
            kCapabilityModel,
            std::string{
                "Particle Distribution"});
        commands.SetProperty(
            ringSystem,
            kRingPlaneNormalBody,
            math::Double3{0.0, 1.0, 0.0});

        const auto outer =
            commands.CreateObject(
                kRingBandType,
                "Outer",
                ringSystem);
        commands.SetProperty(
            outer,
            kCapabilityModel,
            std::string{"Physical Band"});
        commands.SetProperty(
            outer,
            kRingBandInnerRadiusMeters,
            2.4);
        commands.SetProperty(
            outer,
            kRingBandOuterRadiusMeters,
            3.0);
        commands.SetProperty(
            outer,
            kRingBandOpticalDepth,
            0.3);

        const auto inner =
            commands.CreateObject(
                kRingBandType,
                "Inner",
                ringSystem);
        commands.SetProperty(
            inner,
            kCapabilityModel,
            std::string{"Physical Band"});
        commands.SetProperty(
            inner,
            kRingBandInnerRadiusMeters,
            1.8);
        commands.SetProperty(
            inner,
            kRingBandOuterRadiusMeters,
            2.3);
        commands.SetProperty(
            inner,
            kRingBandOpticalDepth,
            1.0);

        const auto resolved =
            ResolveRingSystem(
                objects,
                planet);

        if (!resolved.has_value() ||
            resolved->parameters.bands.size() !=
                2U ||
            resolved->parameters.bands[0].
                innerRadiusMeters !=
                1.8 ||
            resolved->parameters.bands[1].
                innerRadiusMeters !=
                2.4 ||
            resolved->parameters.fingerprint ==
                0U)
        {
            return 1;
        }

        UniverseComposition universe;
        (void)universe.Rebuild(objects);

        const auto starId =
            universe.BodyForObject(star);
        const auto planetId =
            universe.BodyForObject(planet);

        if (!starId.has_value() ||
            !planetId.has_value())
        {
            return 2;
        }

        CelestialLightingService lighting(
            objects,
            universe);

        const auto clear =
            lighting.DirectLightingAtBody(
                *planetId,
                *starId,
                {},
                {});

        const auto ringed =
            lighting.DirectLightingAtSurface(
                *planetId,
                *starId,
                {},
                {},
                resolved->parameters,
                math::Double3{
                    0.9950371902,
                   -0.0995037190,
                    0.0});

        if (!clear.has_value() ||
            !ringed.has_value() ||
            !(ringed->ringTransmittance <
                1.0) ||
            !(ringed->
                  irradianceWattsPerSquareMeter <
              clear->
                  irradianceWattsPerSquareMeter) ||
            ringed->celestial.visibleFraction !=
                clear->visibleFraction ||
            ringed->cloudTransmittance !=
                1.0)
        {
            return 3;
        }

        const u64 before =
            resolved->parameters.fingerprint;

        commands.SetProperty(
            inner,
            kRingBandOpticalDepth,
            2.0);

        const auto revised =
            ResolveRingSystem(
                objects,
                planet);

        if (!revised.has_value() ||
            revised->parameters.fingerprint ==
                before)
        {
            return 4;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
