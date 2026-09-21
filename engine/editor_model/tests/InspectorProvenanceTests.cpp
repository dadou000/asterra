#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/PropertyProvenanceStore.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <variant>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-inspector-provenance-" +
         documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Inspector Provenance Test");

        documents::WorldDatabase world(
            project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        RegisterSchemas(schemas);

        scene::ObjectStore objects(world);
        commands::CommandService commands(
            objects,
            schemas);
        selection::SelectionService selection;

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
                "Body",
                system);

        const auto atmosphere =
            commands.CreateObject(
                kAtmosphereCapabilityType,
                "Atmosphere",
                body);

        commands.SetProperty(
            atmosphere,
            kAtmosphereRayleighScaleHeightMeters,
            8000.0);

        static_cast<void>(
            WritePropertyProvenance(
                objects,
                commands,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters,
                PropertyProvenance{
                    .sourceMode =
                        PropertySourceMode::Derived,
                    .solveState =
                        PropertySolveState::Solved,
                    .sourceProperty =
                        "test derived value"
                }));

        const std::array selected{
            atmosphere
        };
        selection.Set(
            std::span(selected));

        editor_model::InspectorModel inspector(
            objects,
            schemas,
            commands,
            selection);

        inspector.SetForSelection(
            kAtmosphereRayleighScaleHeightMeters,
            9000.0);

        const auto explicitProvenance =
            ReadStoredPropertyProvenance(
                objects,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        if (!explicitProvenance.has_value() ||
            explicitProvenance->sourceMode !=
                PropertySourceMode::Explicit ||
            explicitProvenance->solveState !=
                PropertySolveState::Locked ||
            explicitProvenance->sourceProperty !=
                "Manual Inspector edit")
        {
            return 1;
        }

        const auto changed =
            objects.GetProperty(
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        if (!changed.has_value() ||
            std::get<f64>(*changed) !=
                9000.0)
        {
            return 2;
        }

        commands.Undo();

        const auto reverted =
            objects.GetProperty(
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        const auto revertedProvenance =
            ReadStoredPropertyProvenance(
                objects,
                atmosphere,
                kAtmosphereRayleighScaleHeightMeters);

        if (!reverted.has_value() ||
            std::get<f64>(*reverted) !=
                8000.0 ||
            !revertedProvenance.has_value() ||
            revertedProvenance->sourceMode !=
                PropertySourceMode::Derived ||
            revertedProvenance->solveState !=
                PropertySolveState::Solved)
        {
            return 3;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
