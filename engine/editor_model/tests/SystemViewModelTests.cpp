#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/SystemViewModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-system-view-model-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "System View Model Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::selection::SelectionService selection;

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto system =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldObject);
        const auto body =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Asterra",
                system);
        const auto orbitCapability =
            commands.CreateObject(
                orbit::world_model::kOrbitCapabilityType,
                "Orbit",
                body);

        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Analytic Conic"});
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitSemiMajorAxisMeters,
            10'000.0);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitEccentricity,
            0.0);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitGravitationalParameter,
            1.0e8);

        orbit::world_model::UniverseComposition universe;
        universe.Rebuild(objects);

        orbit::editor_model::SystemViewModel model(
            objects,
            universe,
            selection,
            commands);

        const auto systems =
            model.Systems();

        if (systems.size() != 1U ||
            systems.front().id != system)
        {
            return 1;
        }

        const auto items =
            model.Items(
                system,
                orbit::time::SimulationTime{});

        if (items.size() != 1U ||
            items.front().object != body ||
            items.front().
                distanceFromSystemOriginMeters !=
                10'000.0)
        {
            return 2;
        }

        const auto samples =
            model.SampleTrajectory(
                body,
                system,
                orbit::time::SimulationTime{},
                60.0,
                8U);

        if (samples.size() != 9U ||
            samples.front().
                positionMeters.x !=
                10'000.0)
        {
            return 3;
        }

        model.Select(body);

        const auto selectedSystem =
            model.SystemForSelection();

        if (!selectedSystem.has_value() ||
            selectedSystem->id != system)
        {
            return 4;
        }

        const auto target =
            model.AnalyticOrbitTarget(body);

        if (!target.has_value())
        {
            return 5;
        }

        auto edited =
            target->values;
        edited.semiMajorAxisMeters =
            20'000.0;
        edited.eccentricity =
            0.25;
        edited.periapsisDistanceMeters =
            15'000.0;
        edited.inclinationDegrees =
            12.5;

        model.ApplyAnalyticOrbitEdit(
            target->capability,
            edited);

        const auto editedA =
            objects.GetProperty(
                target->capability,
                orbit::world_model::
                    kOrbitSemiMajorAxisMeters);
        const auto editedE =
            objects.GetProperty(
                target->capability,
                orbit::world_model::
                    kOrbitEccentricity);

        if (!editedA.has_value() ||
            !editedE.has_value() ||
            std::get<double>(*editedA) !=
                20'000.0 ||
            std::get<double>(*editedE) !=
                0.25)
        {
            return 6;
        }

        commands.Undo();

        const auto undoneA =
            objects.GetProperty(
                target->capability,
                orbit::world_model::
                    kOrbitSemiMajorAxisMeters);
        const auto undoneE =
            objects.GetProperty(
                target->capability,
                orbit::world_model::
                    kOrbitEccentricity);

        if (!undoneA.has_value() ||
            !undoneE.has_value() ||
            std::get<double>(*undoneA) !=
                10'000.0 ||
            std::get<double>(*undoneE) !=
                0.0)
        {
            return 7;
        }

        commands.Redo();

        const auto redoneA =
            objects.GetProperty(
                target->capability,
                orbit::world_model::
                    kOrbitSemiMajorAxisMeters);

        if (!redoneA.has_value() ||
            std::get<double>(*redoneA) !=
                20'000.0)
        {
            return 8;
        }

        bool invalidRejected = false;

        try
        {
            auto invalid = edited;
            invalid.eccentricity = -1.0;
            model.ApplyAnalyticOrbitEdit(
                target->capability,
                invalid);
        }
        catch (const std::invalid_argument&)
        {
            invalidRejected = true;
        }

        if (!invalidRejected)
        {
            return 9;
        }

        const auto reference =
            commands.CreateObject(
                orbit::world_model::
                    kCelestialReferenceNodeType,
                "Barycenter",
                system);

        model.SetReferenceNodePosition(
            reference,
            {1.0, 2.0, 3.0});

        const auto referencePosition =
            objects.GetProperty(
                reference,
                orbit::world_model::
                    kReferenceNodePositionMeters);

        if (!referencePosition.has_value() ||
            std::get<orbit::math::Double3>(
                *referencePosition) !=
                orbit::math::Double3{
                    1.0, 2.0, 3.0})
        {
            return 10;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
