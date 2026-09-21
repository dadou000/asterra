#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/CelestialAuthoringModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <stdexcept>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-celestial-authoring-model-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Celestial Authoring Model Test");
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

        orbit::editor_model::CelestialAuthoringModel model(
            objects,
            schemas,
            commands,
            selection);

        const auto system =
            model.CreateSystem("Helion");

        if (!objects.Find(system).has_value())
        {
            return 1;
        }

        const auto body =
            model.CreateBody("Asterra");

        const auto selectedBody =
            model.SelectedBody();

        if (!selectedBody.has_value() ||
            selectedBody->id != body)
        {
            return 2;
        }

        const auto gravity =
            model.AddCapability(
                orbit::world_model::kGravityCapabilityType,
                "Gravity");

        bool duplicateRejected = false;
        try
        {
            static_cast<void>(
                model.AddCapability(
                    orbit::world_model::kGravityCapabilityType,
                    "Gravity 2"));
        }
        catch (const std::runtime_error&)
        {
            duplicateRejected = true;
        }

        if (!duplicateRejected)
        {
            return 3;
        }

        const auto diagnostics =
            model.Validate();

        if (diagnostics.size() != 1U ||
            diagnostics.front().severity !=
                orbit::editor_model::
                    CelestialDiagnosticSeverity::Info)
        {
            return 4;
        }

        const std::array selectedGravity{gravity};
        selection.Set(
            std::span(selectedGravity));

        model.RemoveSelectedCapability();

        if (objects.Find(gravity).has_value())
        {
            return 5;
        }

        commands.Undo();

        if (!objects.Find(gravity).has_value())
        {
            return 6;
        }

        const std::array selectedBodyArray{body};
        selection.Set(
            std::span(selectedBodyArray));

        const auto orbitCapability =
            model.AddCapability(
                orbit::world_model::kOrbitCapabilityType,
                "Orbit");

        const std::array selectedOrbit{orbitCapability};
        selection.Set(
            std::span(selectedOrbit));

        orbit::editor_model::InspectorModel inspector(
            objects,
            schemas,
            commands,
            selection);

        bool foundBasic = false;
        bool foundAdvanced = false;

        for (const auto& property :
             inspector.CommonProperties())
        {
            if (property.schema.id ==
                orbit::world_model::
                    kOrbitEccentricity)
            {
                foundBasic =
                    !property.schema.advanced;
            }

            if (property.schema.id ==
                orbit::world_model::
                    kOrbitEpochMicroseconds)
            {
                foundAdvanced =
                    property.schema.advanced;
            }
        }

        if (!foundBasic ||
            !foundAdvanced)
        {
            return 7;
        }

        const auto bodyRecord =
            objects.Find(body);

        if (!bodyRecord.has_value() ||
            !bodyRecord->parent.has_value() ||
            *bodyRecord->parent != system)
        {
            return 8;
        }

        const auto systemRecord =
            objects.Find(system);

        if (!systemRecord.has_value() ||
            !systemRecord->parent.has_value() ||
            *systemRecord->parent != worldObject)
        {
            return 9;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
