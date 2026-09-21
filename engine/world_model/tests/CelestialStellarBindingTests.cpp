#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/CelestialRadiometryBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <filesystem>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-stellar-binding-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Stellar Binding Test");

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

        const auto emitter =
            commands.CreateObject(
                kRadiativeEmitterCapabilityType,
                "Emitter",
                star);

        commands.SetProperty(
            emitter,
            kCapabilityModel,
            std::string{"Blackbody"});
        commands.SetProperty(
            emitter,
            kEmitterDeriveLuminosity,
            true);
        commands.SetProperty(
            emitter,
            kEmitterEmissivity,
            1.0);

        const auto photosphere =
            commands.CreateObject(
                kPhotosphereCapabilityType,
                "Photosphere",
                star);

        commands.SetProperty(
            photosphere,
            kCapabilityModel,
            std::string{"Blackbody"});
        commands.SetProperty(
            photosphere,
            kPhotosphereRadiusMeters,
            6.957e8);
        commands.SetProperty(
            photosphere,
            kPhotosphereTemperatureKelvin,
            5772.0);

        const auto baseline =
            ResolveRadiativeBody(
                objects,
                star);

        if (!baseline.has_value())
            return 1;

        const auto luminosity =
            baseline->radiative.luminosityWatts;
        const auto temperature =
            baseline->radiative.effectiveTemperatureKelvin;
        const auto radius =
            baseline->photosphereRadiusMeters;
        const auto fingerprint =
            baseline->stellarAppearanceFingerprint;
        const auto color =
            baseline->stellarColorLinear;

        commands.SetProperty(
            photosphere,
            kPhotosphereLimbDarkening,
            0.31);
        commands.SetProperty(
            photosphere,
            kPhotosphereGranulationStrength,
            0.42);
        commands.SetProperty(
            photosphere,
            kPhotosphereCoronaStrength,
            0.19);
        commands.SetProperty(
            photosphere,
            kPhotosphereGlareRadiusPixels,
            9.0);

        const auto appearanceEdit =
            ResolveRadiativeBody(
                objects,
                star);

        if (!appearanceEdit.has_value() ||
            appearanceEdit->stellarAppearanceFingerprint ==
                fingerprint ||
            appearanceEdit->radiative.luminosityWatts !=
                luminosity ||
            appearanceEdit->radiative.effectiveTemperatureKelvin !=
                temperature ||
            appearanceEdit->photosphereRadiusMeters !=
                radius ||
            appearanceEdit->stellarColorLinear.x !=
                color.x ||
            appearanceEdit->stellarColorLinear.y !=
                color.y ||
            appearanceEdit->stellarColorLinear.z !=
                color.z)
        {
            return 2;
        }

        commands.SetProperty(
            photosphere,
            kPhotosphereTemperatureKelvin,
            3200.0);

        const auto temperatureEdit =
            ResolveRadiativeBody(
                objects,
                star);

        if (!temperatureEdit.has_value() ||
            temperatureEdit->radiative.effectiveTemperatureKelvin !=
                3200.0 ||
            temperatureEdit->radiative.luminosityWatts ==
                luminosity ||
            !(temperatureEdit->stellarColorLinear.x >
              temperatureEdit->stellarColorLinear.z))
        {
            return 3;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
