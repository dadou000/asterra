#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio runtime binding test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId AddBody(
    orbit::studio_session::StudioSession& studio,
    const std::string_view name,
    const double radius)
{
    auto& commands = studio.World().Commands();
    const auto world =
        commands.CreateObject(
            orbit::world_model::kWorldType,
            "World");
    const auto system =
        commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            world);
    const auto body =
        commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            name,
            system);
    commands.SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    commands.SetProperty(
        body,
        orbit::world_model::kBodyMass,
        5.0e24);
    return body;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-runtime-binding-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Runtime Binding Test");
        orbit::studio_session::StudioSession studio(project);
        orbit::studio_session::StudioRuntimeBinding runtime(studio);

        const auto blank = runtime.Refresh();
        Check(blank.hasWorld);
        Check(runtime.IsCurrent(blank));
        Check(!blank.activeBody.has_value());
        Check(runtime.ActiveBodyRecord(blank) == std::nullopt);
        static_cast<void>(runtime.Frames(blank));
        static_cast<void>(runtime.Bodies(blank));
        static_cast<void>(runtime.Routes(blank));
        Check(runtime.PathProducts(blank).Empty());

        const auto asterra =
            AddBody(
                studio,
                "Asterra",
                6'000'000.0);
        const auto composed = runtime.Refresh();
        Check(runtime.IsCurrent(composed));
        Check(composed.compositionChanged);
        Check(composed.activeBody.has_value());
        Check(
            composed.activeBody->semanticObject ==
            asterra);
        Check(
            composed.activeBody->universeGeneration ==
            composed.universeGeneration);

        const auto body =
            runtime.ActiveBodyRecord(composed);
        Check(body.has_value());
        Check(body->name == "Asterra");
        Check(body->id == composed.activeBody->body);
        Check(body->frame == composed.activeBody->frame);

        const auto captured = runtime.Capture();
        Check(runtime.IsCurrent(captured));
        Check(captured.activeBody.has_value());
        Check(
            captured.universeGeneration ==
            composed.universeGeneration);

        auto& worldCommands =
            studio.World().Commands();

        const auto terrain =
            worldCommands.CreateObject(
                orbit::world_model::
                    kTerrainSurfaceType,
                "Asterra Terrain",
                asterra);

        worldCommands.SetProperty(
            terrain,
            orbit::world_model::kTerrainSeed,
            orbit::i64{424242});

        const auto terrainComposed =
            runtime.Refresh();

        Check(
            terrainComposed.
                compositionChanged);
        Check(
            terrainComposed.
                universeGeneration >
            composed.universeGeneration);
        Check(
            studio.World().
                SurfaceStats().
                terrainSurfaces ==
            1U);

        const auto terrainBody =
            studio.World().Universe().
                BodyForObject(
                    asterra);
        Check(terrainBody.has_value());

        const auto* capability =
            studio.World().Surfaces().
                Registry().
                FindTerrainSurface(
                    *terrainBody);
        Check(capability != nullptr);

        const auto* analytic =
            dynamic_cast<
                const orbit::terrain::
                    AnalyticTerrainSource*>(
                        capability->
                            terrain.get());
        Check(analytic != nullptr);
        Check(
            analytic->Description().
                macroAmplitudeMeters ==
            1'200.0);

        worldCommands.SetProperty(
            terrain,
            orbit::world_model::
                kTerrainMacroAmplitudeMeters,
            3'333.0);

        // Before Refresh, the current runtime composition remains immutable.
        Check(
            analytic->Description().
                macroAmplitudeMeters ==
            1'200.0);
        Check(
            runtime.IsCurrent(
                terrainComposed));

        const auto terrainEdited =
            runtime.Refresh();

        Check(
            terrainEdited.
                compositionChanged);
        Check(
            terrainEdited.
                universeGeneration >
            terrainComposed.
                universeGeneration);
        Check(
            !runtime.IsCurrent(
                terrainComposed));

        const auto* editedCapability =
            studio.World().Surfaces().
                Registry().
                FindTerrainSurface(
                    *terrainBody);
        Check(
            editedCapability !=
            nullptr);

        const auto* editedAnalytic =
            dynamic_cast<
                const orbit::terrain::
                    AnalyticTerrainSource*>(
                        editedCapability->
                            terrain.get());

        Check(editedAnalytic != nullptr);
        Check(
            editedAnalytic->
                Description().
                macroAmplitudeMeters ==
            3'333.0);

        const auto idleAfterTerrainEdit =
            runtime.Refresh();

        Check(
            !idleAfterTerrainEdit.
                compositionChanged);
        Check(
            idleAfterTerrainEdit.
                universeGeneration ==
            terrainEdited.
                universeGeneration);

        studio.World().Commands().SetProperty(
            asterra,
            orbit::world_model::kBodyRadius,
            6'100'000.0);

        // Until Refresh advances composition, the current runtime state is
        // intentionally still the previous immutable composition.
        Check(runtime.IsCurrent(composed));

        const auto edited = runtime.Refresh();
        Check(runtime.IsCurrent(edited));
        Check(edited.compositionChanged);
        Check(
            edited.universeGeneration >
            terrainEdited.universeGeneration);
        Check(!runtime.IsCurrent(composed));
        Check(edited.pathRoutingRebound);
        Check(edited.pathProductsInvalidated);

        bool staleFramesRejected = false;
        try
        {
            static_cast<void>(runtime.Frames(composed));
        }
        catch (const std::logic_error&)
        {
            staleFramesRejected = true;
        }
        Check(staleFramesRejected);

        bool staleBodyRejected = false;
        try
        {
            static_cast<void>(
                runtime.ActiveBodyRecord(composed));
        }
        catch (const std::logic_error&)
        {
            staleBodyRejected = true;
        }
        Check(staleBodyRejected);

        const auto editedBody =
            runtime.ActiveBodyRecord(edited);
        Check(editedBody.has_value());
        Check(
            std::get<orbit::universe::SphereShape>(
                editedBody->shape).radiusMeters ==
            6'100'000.0);

        const auto secondary =
            studio.CreateWorld(
                "Secondary",
                "Secondary");
        studio.OpenWorld(secondary.relativePath);
        const auto switched = runtime.Refresh();
        Check(runtime.IsCurrent(switched));
        Check(switched.hasWorld);
        Check(!switched.activeBody.has_value());
        Check(!runtime.IsCurrent(edited));

        studio.CloseWorld();
        const auto closed = runtime.Refresh();
        Check(runtime.IsCurrent(closed));
        Check(!closed.hasWorld);
        Check(!closed.activeBody.has_value());

        bool closedBodiesRejected = false;
        try
        {
            static_cast<void>(runtime.Bodies(closed));
        }
        catch (const std::logic_error&)
        {
            closedBodiesRejected = true;
        }
        Check(closedBodiesRejected);
    }

    std::filesystem::remove_all(root);
    return 0;
}
