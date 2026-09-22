#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <variant>

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-volume-command-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Volume Command Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(objects, schemas);
        orbit::selection::SelectionService selection;
        orbit::commands::CommandRegistry registry;

        orbit::editor_model::authoring_commands::
            RegisterVolumeCommands(
                registry,
                commands,
                objects,
                selection);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto systemObject =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "System",
                worldObject);
        const auto bodyObject =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Body",
                systemObject);

        const orbit::scene::ObjectId selectedBody[]{
            bodyObject};
        selection.Set(selectedBody);

        orbit::commands::CommandArguments args;
        args.emplace(
            "preset",
            std::string{"Fire"});

        registry.Invoke(
            orbit::editor_model::authoring_commands::
                kCreateVolume,
            args);

        if (selection.Ordered().size() != 1U)
            return 1;

        const auto volume =
            selection.Ordered().front();

        const auto resolved =
            orbit::world_model::
                ResolveVolumeDomain(
                    objects,
                    volume);

        if (!resolved.has_value() ||
            resolved->preset != "Fire" ||
            resolved->solverPolicy !=
                orbit::world_model::
                    VolumeSolverPolicy::Local3D ||
            (resolved->fieldMask &
             static_cast<orbit::u64>(
                 orbit::world_model::
                     VolumeField::Emission)) == 0U)
            return 2;

        const auto schema =
            schemas.FindType(
                orbit::world_model::kVolumeType);

        if (schema == nullptr ||
            schema->properties.size() < 8U)
            return 3;

        {
            orbit::commands::CommandArguments sourceArgs;
            sourceArgs.emplace(
                "kind",
                std::string{"Brush"});

            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kAddVolumeSource,
                sourceArgs);

            if (selection.Ordered().size() != 1U)
                return 20;

            const auto brush =
                selection.Ordered().front();

            const auto firstInputs =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            if (firstInputs.size() != 1U ||
                firstInputs.front().object != brush ||
                firstInputs.front().kind !=
                    static_cast<orbit::i64>(
                        orbit::world_model::
                            VolumeSourceKind::Brush))
                return 21;

            selection.Set(
                std::span(
                    &volume,
                    1U));

            orbit::commands::CommandArguments splineArgs;
            splineArgs.emplace(
                "kind",
                std::string{"Spline"});

            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kAddVolumeSource,
                splineArgs);

            const auto spline =
                selection.Ordered().front();

            auto ordered =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            if (ordered.size() != 2U ||
                ordered[0].object != brush ||
                ordered[1].object != spline)
                return 22;

            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kMoveVolumeInputUp);

            ordered =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            if (ordered[0].object != spline ||
                ordered[1].object != brush)
                return 23;

            const auto oldFingerprint =
                ordered[0].fingerprint;
            const auto oldBounds =
                ordered[0].bounds;

            commands.SetProperty(
                spline,
                orbit::world_model::
                    kVolumeChildPositionMeters,
                orbit::math::Double3{
                    5.0, 2.0, -3.0});

            const auto moved =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            const auto movedSpline =
                std::find_if(
                    moved.begin(),
                    moved.end(),
                    [spline](const auto& input)
                    {
                        return input.object ==
                            spline;
                    });

            if (movedSpline == moved.end() ||
                movedSpline->fingerprint ==
                    oldFingerprint ||
                movedSpline->bounds.minimumMeters ==
                    oldBounds.minimumMeters)
                return 24;

            selection.Set(
                std::span(
                    &volume,
                    1U));

            orbit::commands::CommandArguments paintArgs;
            paintArgs.emplace(
                "position",
                orbit::math::Double3{
                    10.0, 0.0, 12.0});
            paintArgs.emplace(
                "radius",
                3.0);
            paintArgs.emplace(
                "strength",
                0.75);

            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kPaintVolumeTerrainSource,
                paintArgs);

            const auto terrainStroke =
                selection.Ordered().front();

            const auto withStroke =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            const auto stroke =
                std::find_if(
                    withStroke.begin(),
                    withStroke.end(),
                    [terrainStroke](const auto& input)
                    {
                        return input.object ==
                            terrainStroke;
                    });

            if (stroke == withStroke.end() ||
                stroke->kind !=
                    static_cast<orbit::i64>(
                        orbit::world_model::
                            VolumeSourceKind::Terrain) ||
                !stroke->paintEnabled ||
                stroke->shape !=
                    orbit::world_model::
                        VolumeSourceShape::
                            TerrainPatch ||
                stroke->radiusMeters != 3.0 ||
                stroke->scalarValue != 0.75)
                return 25;

            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kRemoveVolumeInput);

            if (objects.Find(
                    terrainStroke).
                    has_value())
                return 26;

            commands.Undo();

            if (!objects.Find(
                    terrainStroke).
                    has_value())
                return 27;

            const auto restoredInputs =
                orbit::world_model::
                    ResolveVolumeInputs(
                        objects,
                        volume);

            const auto restoredStroke =
                std::find_if(
                    restoredInputs.begin(),
                    restoredInputs.end(),
                    [terrainStroke](const auto& input)
                    {
                        return input.object ==
                            terrainStroke;
                    });

            if (restoredStroke ==
                    restoredInputs.end() ||
                restoredStroke->scalarValue != 0.75)
                return 28;

            // Remove the input children again so the existing Volume-remove
            // regression remains valid for a leaf-only CommandService delete.
            selection.Set(
                std::span(
                    &terrainStroke,
                    1U));
            registry.Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kRemoveVolumeInput);

            const scene::ObjectId sourceIds[]{
                spline,
                brush
            };

            for (const auto source :
                 sourceIds)
            {
                const scene::ObjectId selectedSource[]{
                    source};
                selection.Set(selectedSource);
                registry.Invoke(
                    orbit::editor_model::
                        authoring_commands::
                            kRemoveVolumeInput);
            }

            selection.Set(
                std::span(
                    &volume,
                    1U));
        }

        if (!registry.Enablement(
                orbit::editor_model::
                    authoring_commands::
                        kRemoveVolume).enabled)
            return 4;

        registry.Invoke(
            orbit::editor_model::authoring_commands::
                kRemoveVolume);

        if (objects.Find(volume).has_value())
            return 5;

        if (!commands.CanUndo())
            return 6;

        commands.Undo();

        const auto restored =
            orbit::world_model::
                ResolveVolumeDomain(
                    objects,
                    volume);

        if (!restored.has_value() ||
            restored->preset != "Fire")
            return 7;

        world.Checkpoint();
    }

    // Reopen the same world and prove authored Volume state survives the
    // ordinary document persistence path rather than a volumetric sidecar.
    {
        auto project =
            orbit::documents::ProjectDocument::Open(
                root / "Project.orbit.toml");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::scene::ObjectStore objects(world);

        bool foundFire = false;

        for (const auto& rootObject :
             objects.Roots())
        {
            for (const auto& system :
                 objects.Children(rootObject.id))
            {
                for (const auto& body :
                     objects.Children(system.id))
                {
                    for (const auto& child :
                         objects.Children(body.id))
                    {
                        const auto resolved =
                            orbit::world_model::
                                ResolveVolumeDomain(
                                    objects,
                                    child.id);

                        if (resolved.has_value() &&
                            resolved->preset == "Fire")
                        {
                            foundFire = true;
                        }
                    }
                }
            }
        }

        if (!foundFire)
            return 8;
    }

    std::filesystem::remove_all(root);
    return 0;
}
