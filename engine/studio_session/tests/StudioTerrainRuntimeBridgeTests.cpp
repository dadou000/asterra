#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain runtime bridge test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId CreateRockyPlanet(
    orbit::studio_session::StudioSession& studio,
    const orbit::scene::ObjectId worldObject,
    const std::string& name)
{
    const orbit::scene::ObjectId selected[] = {
        worldObject
    };

    studio.World().Selection().Set(
        selected);

    studio.World().CommandRegistry().Invoke(
        orbit::editor_model::
            authoring_commands::
                kCreateRockyPlanet,
        {
            {
                "name",
                name
            }
        });

    Check(
        studio.World().Selection().
            Ordered().size() ==
        1U);

    return
        studio.World().Selection().
            Ordered().front();
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-terrain-runtime-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(
        root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Terrain Runtime Test");

        orbit::studio_session::StudioSession
            studio(project);

        studio.Viewports().Register(
            "studio.primary",
            orbit::studio_session::
                ViewportMode::Perspective,
            true);

        static_cast<void>(
            studio.Tick());

        Check(
            !studio.TerrainRuntime().
                Capture(
                    "studio.primary").
                has_value());

        const auto worldObject =
            studio.World().Commands().
                CreateObject(
                    orbit::world_model::kWorldType,
                    "World");

        const auto asterra =
            CreateRockyPlanet(
                studio,
                worldObject,
                "Asterra");

        const auto composed =
            studio.Tick();

        Check(
            composed.compositionChanged);
        Check(
            composed.terrainRuntimeChanged);

        const auto primary =
            studio.TerrainRuntime().
                Capture(
                    "studio.primary");

        Check(primary.has_value());
        Check(
            primary->semanticBody ==
                asterra);
        Check(
            primary->worldGeneration ==
                studio.World().
                    Generation());
        Check(
            primary->universeGeneration ==
                studio.World().
                    UniverseGeneration());
        Check(
            primary->planet.id ==
                primary->
                    observerPhysicalPage.
                    planet);
        Check(
            primary->physicalPageLevel ==
                primary->
                    observerPhysicalPage.
                    tile.level);
        Check(
            primary->layout.levels.size() ==
                primary->clipmap.
                    levelCount);
        Check(
            primary->motion.levels.size() ==
                primary->clipmap.
                    levelCount);
        Check(
            primary->residency.levels.size() ==
                primary->clipmap.
                    levelCount);
        Check(
            !primary->sampleRequests.empty());

        const auto& source =
            studio.TerrainRuntime().
                TerrainSource(
                    *primary);

        Check(
            source.Revision() ==
                primary->
                    terrainSourceRevision);

        auto* composedCache =
            studio.World().Surfaces().
                CacheForBody(
                    primary->body);

        Check(
            composedCache != nullptr);
        Check(
            &studio.TerrainRuntime().
                Cache(
                    *primary) ==
            composedCache);

        // A second viewport targeting the same semantic body gets its own
        // presentation runtime while sharing physical/cache authority.
        studio.Viewports().Register(
            "studio.secondary",
            orbit::studio_session::
                ViewportMode::Debug,
            true);

        const auto secondTick =
            studio.Tick();

        Check(
            secondTick.
                terrainRuntimeChanged);

        const auto secondary =
            studio.TerrainRuntime().
                Capture(
                    "studio.secondary");

        Check(secondary.has_value());
        Check(
            secondary->body ==
                primary->body);
        Check(
            studio.TerrainRuntime().
                Catalog().size() ==
            2U);
        Check(
            secondary->
                observerPhysicalPage ==
            primary->
                observerPhysicalPage);

        // Camera/observer movement is presentation state only.
        const orbit::u64
            universeBeforeMove =
                studio.World().
                    UniverseGeneration();

        const orbit::u64
            sourceRevisionBeforeMove =
                primary->
                    terrainSourceRevision;

        const auto movedDirection =
            orbit::math::Normalize(
                orbit::math::Double3{
                    0.25,
                    0.91,
                    -0.31
                });

        const orbit::world::WorldPosition
            movedObserver{
                .meters =
                    movedDirection *
                    (primary->planet.
                         radiusMeters +
                     25'000.0)
            };

        Check(
            studio.TerrainRuntime().
                SetObserver(
                    "studio.primary",
                    movedObserver));

        const auto moved =
            studio.TerrainRuntime().
                Capture(
                    "studio.primary");

        Check(moved.has_value());
        Check(
            moved->runtimeGeneration >
                primary->
                    runtimeGeneration);
        Check(
            moved->universeGeneration ==
                universeBeforeMove);
        Check(
            moved->terrainSourceRevision ==
                sourceRevisionBeforeMove);
        Check(
            moved->observerPhysicalPage !=
                primary->
                    observerPhysicalPage);
        Check(
            !studio.TerrainRuntime().
                IsCurrent(
                    *primary));
        Check(
            studio.TerrainRuntime().
                IsCurrent(
                    *moved));

        // M03 establishes the generation-checked M29 publication seam.
        orbit::terrain_debug::
            TerrainDebugPageStamp
            debugStamp{
                .address =
                    moved->
                        observerPhysicalPage,
                .physicalLod = 2U,
                .revisions = {},
                .cacheResident = false,
                .invalidationRevision =
                    moved->
                        surfaceSourceRevision
            };

        auto debugPage =
            std::make_shared<
                orbit::terrain_debug::
                    TerrainDebugPageData>(
                        debugStamp,
                        1U,
                        1U);

        studio.TerrainRuntime().
            PublishDebugPage(
                *moved,
                debugPage);

        Check(
            studio.TerrainDebugPages().
                Find(
                    moved->
                        observerPhysicalPage) ==
            debugPage);

        // Semantic terrain edits rebuild SurfaceComposition, preserve the
        // observer value, and make the old runtime snapshot unusable.
        const auto terrainObject =
            moved->terrainObject;

        studio.World().Commands().
            SetProperty(
                terrainObject,
                orbit::world_model::
                    kTerrainMacroAmplitudeMeters,
                2'222.0);

        const auto editedTick =
            studio.Tick();

        Check(
            editedTick.compositionChanged);
        Check(
            editedTick.
                terrainRuntimeChanged);

        const auto edited =
            studio.TerrainRuntime().
                Capture(
                    "studio.primary");

        Check(edited.has_value());
        Check(
            edited->universeGeneration >
                moved->
                    universeGeneration);
        Check(
            edited->observer.meters ==
                moved->observer.meters);
        Check(
            edited->
                observerPhysicalPage ==
            moved->
                observerPhysicalPage);
        Check(
            !studio.TerrainRuntime().
                IsCurrent(
                    *moved));
        Check(
            studio.TerrainRuntime().
                IsCurrent(
                    *edited));

        bool staleCacheRejected = false;

        try
        {
            static_cast<void>(
                studio.TerrainRuntime().
                    Cache(
                        *moved));
        }
        catch (const std::logic_error&)
        {
            staleCacheRejected = true;
        }

        Check(
            staleCacheRejected);

        // Viewport destruction releases exactly that presentation runtime.
        Check(
            studio.Viewports().
                Unregister(
                    "studio.secondary"));

        const auto unregisterTick =
            studio.Tick();

        Check(
            unregisterTick.
                terrainRuntimeChanged);
        Check(
            !studio.TerrainRuntime().
                Capture(
                    "studio.secondary").
                has_value());
        Check(
            studio.TerrainRuntime().
                Catalog().size() ==
            1U);

        // Recreating a viewport in the same world/body preserves its
        // presentation observer. Physical page identity therefore remains
        // identical and still contains no viewport or clipmap-slot identity.
        const auto pageBeforeRecreate =
            edited->
                observerPhysicalPage;

        const auto observerBeforeRecreate =
            edited->
                observer;

        static_cast<void>(
            studio.Viewports().
                Unregister(
                    "studio.primary"));

        static_cast<void>(
            studio.Tick());

        Check(
            studio.TerrainRuntime().
                Catalog().empty());

        studio.Viewports().Register(
            "studio.primary",
            orbit::studio_session::
                ViewportMode::Perspective,
            true);

        const auto recreatedTick =
            studio.Tick();

        Check(
            recreatedTick.
                terrainRuntimeChanged);

        const auto recreated =
            studio.TerrainRuntime().
                Capture(
                    "studio.primary");

        Check(recreated.has_value());
        Check(
            recreated->observer.meters ==
                observerBeforeRecreate.
                    meters);
        Check(
            recreated->
                observerPhysicalPage ==
            pageBeforeRecreate);

        // A non-terrain body is a valid viewport target but has no terrain
        // runtime bridge.
        const auto system =
            studio.World().Objects().
                Find(asterra)->
                parent;

        Check(system.has_value());

        const auto moon =
            studio.World().Commands().
                CreateObject(
                    orbit::world_model::
                        kCelestialBodyType,
                    "Luma",
                    *system);

        studio.World().Commands().
            SetProperty(
                moon,
                orbit::world_model::
                    kBodyRadius,
                1'500'000.0);

        studio.World().Commands().
            SetProperty(
                moon,
                orbit::world_model::
                    kBodyMass,
                7.0e22);

        const orbit::scene::ObjectId
            moonSelection[] = {
                moon
            };

        studio.World().Selection().Set(
            moonSelection);

        static_cast<void>(
            studio.Tick());

        Check(
            studio.ActiveBody().Active().
                has_value());
        Check(
            studio.ActiveBody().Active()->
                semanticObject ==
            moon);
        Check(
            !studio.TerrainRuntime().
                Capture(
                    "studio.primary").
                has_value());

        // World replacement destroys every viewport terrain runtime and the
        // generation-cleared M29 page publication state.
        const auto secondaryWorld =
            studio.CreateWorld(
                "Secondary",
                "Secondary");

        studio.OpenWorld(
            secondaryWorld.
                relativePath);

        const auto switched =
            studio.Tick();

        Check(
            switched.terrainRuntimeChanged ||
            studio.TerrainRuntime().
                Catalog().empty());
        Check(
            studio.TerrainRuntime().
                Catalog().empty());
        Check(
            studio.TerrainDebugPages().
                Size() ==
            0U);
    }

    std::filesystem::remove_all(
        root);

    return 0;
}
