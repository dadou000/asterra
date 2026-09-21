#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/AtmospherePropertySolver.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/PropertyProvenanceStore.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <variant>

namespace
{
template <typename T>
T RequireProperty(
    const orbit::scene::ObjectStore& objects,
    const orbit::scene::ObjectId object,
    const orbit::schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Required test property is missing.");
    }

    return std::get<T>(*value);
}
}

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-atmosphere-solver-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Atmosphere Solver Test");

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
                "Planet",
                system);

        constexpr f64 radius =
            6.371e6;

        commands.SetProperty(
            body,
            kBodyRadius,
            radius);
        commands.SetProperty(
            body,
            kBodyMass,
            5.9722e24);

        const auto atmosphere =
            commands.CreateObject(
                kAtmosphereCapabilityType,
                "Atmosphere",
                body);

        commands.SetProperty(
            atmosphere,
            kCapabilityModel,
            std::string{
                "Physical Scattering"});

        // A raw expert field authored before provenance exists is
        // conservatively Explicit/Locked and must survive solving.
        constexpr f64 expertTopRadius =
            radius + 200000.0;

        commands.SetProperty(
            atmosphere,
            kAtmosphereTopRadiusMeters,
            expertTopRadius);

        AtmospherePropertySolver solver(
            objects,
            commands);

        const auto preset =
            solver.ApplyPreset(
                atmosphere,
                "Earth-like");

        if (preset.HasInvalidInput() ||
            !preset.HasConflict() ||
            preset.DerivedCount() < 5U ||
            RequireProperty<f64>(
                objects,
                atmosphere,
                kAtmosphereTopRadiusMeters) !=
                expertTopRadius)
        {
            return 1;
        }

        const f64 scaleHeight =
            RequireProperty<f64>(
                objects,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        if (!(scaleHeight > 7000.0) ||
            !(scaleHeight < 10000.0))
        {
            return 2;
        }

        const auto provenance =
            ReadStoredPropertyProvenance(
                objects,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        if (!provenance.has_value() ||
            provenance->sourceMode !=
                PropertySourceMode::Derived ||
            provenance->solveState !=
                PropertySolveState::Solved ||
            provenance->sourceProperty.empty())
        {
            return 3;
        }

        const auto resolved =
            ResolveAtmosphereBody(
                objects,
                body);

        if (!resolved.has_value())
        {
            return 4;
        }

        const u64 firstFingerprint =
            celestial_atmosphere::
                AtmosphereFingerprint(
                    resolved->parameters);

        // ApplyPreset + all solver/provenance writes are one undo unit.
        commands.Undo();

        if (objects.GetProperty(
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters).
                has_value() ||
            ReadStoredPropertyProvenance(
                objects,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters).
                has_value() ||
            RequireProperty<f64>(
                objects,
                atmosphere,
                kAtmosphereTopRadiusMeters) !=
                expertTopRadius)
        {
            return 5;
        }

        commands.Redo();

        if (!objects.GetProperty(
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters).
                has_value())
        {
            return 6;
        }

        commands.SetProperty(
            atmosphere,
            kAtmosphereSurfacePressurePascals,
            70000.0);

        const auto changed =
            solver.Solve(
                atmosphere);

        if (changed.HasInvalidInput())
        {
            return 7;
        }

        const auto revised =
            ResolveAtmosphereBody(
                objects,
                body);

        if (!revised.has_value() ||
            celestial_atmosphere::
                AtmosphereFingerprint(
                    revised->parameters) ==
                firstFingerprint)
        {
            return 8;
        }

        commands.SetProperty(
            atmosphere,
            kAtmosphereMieAnisotropy,
            0.45);

        static_cast<void>(
            WritePropertyProvenance(
                objects,
                commands,
                atmosphere,
                kAtmosphereMieAnisotropy,
                PropertyProvenance{
                    .sourceMode =
                        PropertySourceMode::Imported,
                    .solveState =
                        PropertySolveState::Locked,
                    .sourceAsset =
                        "test://imported-aerosol-profile",
                    .sourceProperty =
                        "Imported aerosol phase profile"
                }));

        const auto importedConflict =
            solver.Solve(
                atmosphere);

        if (!importedConflict.HasConflict() ||
            RequireProperty<f64>(
                objects,
                atmosphere,
                kAtmosphereMieAnisotropy) !=
                0.45)
        {
            return 9;
        }

        const auto rayleigh =
            RequireProperty<math::Double3>(
                objects,
                atmosphere,
                kAtmosphereRayleighScatteringPerMeter);

        commands.SetProperty(
            atmosphere,
            kAtmosphereAuthoringMode,
            std::string{
                "Expert Coefficients"});

        commands.SetProperty(
            atmosphere,
            kAtmosphereSurfacePressurePascals,
            20000.0);

        const auto expert =
            solver.Solve(
                atmosphere);

        if (expert.HasInvalidInput() ||
            expert.DerivedCount() != 0U ||
            RequireProperty<math::Double3>(
                objects,
                atmosphere,
                kAtmosphereRayleighScatteringPerMeter) !=
                rayleigh)
        {
            return 10;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
