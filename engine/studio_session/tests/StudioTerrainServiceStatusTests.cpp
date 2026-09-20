#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain service status test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId CreateRockyPlanet(
    orbit::studio_session::StudioSession& studio,
    const orbit::scene::ObjectId worldObject)
{
    const orbit::scene::ObjectId selected[] = {
        worldObject
    };

    studio.World().Selection().Set(selected);

    studio.World().CommandRegistry().Invoke(
        orbit::editor_model::authoring_commands::kCreateRockyPlanet,
        {
            {"name", std::string("Asterra")}
        });

    Check(
        studio.World().Selection().Ordered().size() ==
        1U);

    return
        studio.World().Selection().Ordered().front();
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-terrain-status-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Terrain Status Test");

        orbit::studio_session::StudioSession studio(project);

        studio.Viewports().Register(
            "studio.primary",
            orbit::studio_session::ViewportMode::Perspective,
            true);

        const auto worldObject =
            studio.World().Commands().CreateObject(
                orbit::world_model::kWorldType,
                "World");

        const auto asterra =
            CreateRockyPlanet(
                studio,
                worldObject);

        static_cast<void>(studio.Tick());

        Check(
            studio.ActiveBody().Active().has_value());

        const auto body =
            studio.ActiveBody().Active()->body;

        const auto terrainObject =
            studio.World().Surfaces().
                TerrainObjectForBody(body);

        Check(terrainObject.has_value());

        const auto status =
            orbit::studio_session::
                StudioTerrainStatusInspector::Capture(
                    studio,
                    *terrainObject,
                    nullptr,
                    "studio.primary");

        Check(status.has_value());
        Check(status->body == body);
        Check(status->terrainObject == *terrainObject);
        Check(
            status->semanticRevision ==
            studio.World().Objects().Revision());
        Check(
            status->surfaceSourceRevision ==
            studio.World().Surfaces().SourceRevision());
        Check(status->defaultBedrock.IsValid());
        Check(status->baseBiome.IsValid());
        Check(status->optionalBiomeCount == 0U);
        Check(status->selectedPhysicalPage.has_value());
        Check(status->selectedPhysicalLod.has_value());
        Check(
            status->selectedViewport ==
            "studio.primary");
        Check(!status->rebuildSchedulerAttached);

        const auto runtime =
            studio.TerrainRuntime().Capture(
                "studio.primary");

        Check(runtime.has_value());

        auto* services =
            studio.World().Surfaces().
                ServicesForBody(body);

        Check(services != nullptr);

        const orbit::terrain_gpu::PersistentGpuTerrainCacheKey key{
            .address =
                runtime->observerPhysicalPage,
            .physicalLod =
                runtime->physicalPageLevel,
            .revisions = {}
        };

        Check(
            services->Cache().Find(key) ==
            nullptr);

        const auto afterMiss =
            orbit::studio_session::
                StudioTerrainStatusInspector::Capture(
                    studio,
                    *terrainObject);

        Check(afterMiss.has_value());
        Check(afterMiss->cacheStats.misses == 1U);

        const orbit::u64 beforeSemanticEdit =
            afterMiss->semanticRevision;

        studio.World().Commands().RenameObject(
            *terrainObject,
            "Terrain Surface Edited");

        const auto editTick =
            studio.Tick();

        Check(editTick.compositionChanged);

        const auto afterEdit =
            orbit::studio_session::
                StudioTerrainStatusInspector::Capture(
                    studio,
                    *terrainObject);

        Check(afterEdit.has_value());
        Check(
            afterEdit->semanticRevision >
            beforeSemanticEdit);
        Check(
            afterEdit->surfaceSourceRevision ==
            afterEdit->semanticRevision);

        // SurfaceComposition owns derived service lifetime. A semantic rebuild
        // recreates the M26 cache instead of persisting runtime residency.
        Check(afterEdit->cacheStats.residentPages == 0U);
        Check(afterEdit->cacheStats.residentBytes == 0U);
        Check(afterEdit->cacheStats.misses == 0U);

        studio.World().Checkpoint();
        studio.CloseWorld();
        studio.OpenWorld("Main");
        static_cast<void>(studio.Tick());

        Check(
            studio.ActiveBody().Active().has_value());
        Check(
            studio.ActiveBody().Active()->
                semanticObject ==
            asterra);

        const auto reopenedBody =
            studio.ActiveBody().Active()->body;
        const auto reopenedTerrain =
            studio.World().Surfaces().
                TerrainObjectForBody(
                    reopenedBody);

        Check(reopenedTerrain.has_value());

        const auto reopened =
            orbit::studio_session::
                StudioTerrainStatusInspector::Capture(
                    studio,
                    *reopenedTerrain);

        Check(reopened.has_value());
        Check(reopened->cacheStats.residentPages == 0U);
        Check(reopened->cacheStats.residentBytes == 0U);
        Check(reopened->cacheStats.hits == 0U);
        Check(reopened->cacheStats.misses == 0U);
    }

    std::filesystem::remove_all(root);
    return 0;
}
