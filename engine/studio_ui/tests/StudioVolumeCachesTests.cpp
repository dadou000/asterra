#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/studio_ui/StudioVolumeCaches.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

int Run()
{
    using namespace orbit;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-volume-caches-" + documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project = documents::ProjectDocument::Create(root, "Volume Cache Test");
        documents::WorldDatabase world(project.StartupWorldPath());
        schema::SchemaRegistry schemas;
        world_model::RegisterSchemas(schemas);

        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);

        const auto worldObject = commands.CreateObject(world_model::kWorldType, "World");
        const auto volume =
            commands.CreateObject(world_model::kVolumeType, "Storm", worldObject);

        // No path yet: nothing to attach.
        if (studio_ui::SyncVolumeCaches(objects, root) != 0U)
        {
            return 1;
        }

        volume_representation::VolumeCacheData cache;
        cache.descriptor.resolutionX = 2U;
        cache.descriptor.resolutionY = 2U;
        cache.descriptor.resolutionZ = 2U;
        cache.density.assign(8U, 0.75F);
        volume_representation::FinalizeVolumeCache(cache);

        const std::string relative = "Content/Volumes/storm.orbitvol";
        std::filesystem::create_directories((root / relative).parent_path());
        std::string error;
        if (!volume_representation::SaveVolumeCache(
                (root / relative).string(), cache, &error))
        {
            return 2;
        }

        commands.SetProperty(volume, world_model::kVolumeCachePath, relative);

        if (studio_ui::SyncVolumeCaches(objects, root) != 1U ||
            !volume_representation::VolumeCaches().Has(volume))
        {
            return 3;
        }

        // Already attached: not loaded again.
        if (studio_ui::SyncVolumeCaches(objects, root) != 0U)
        {
            return 4;
        }

        // A missing file is reported once and then skipped.
        volume_representation::VolumeCaches().Detach(volume);
        commands.SetProperty(volume, world_model::kVolumeCachePath, std::string("Content/Volumes/missing.orbitvol"));
        std::string log;
        if (studio_ui::SyncVolumeCaches(objects, root, &log) != 0U || log.empty())
        {
            return 5;
        }
        log.clear();
        if (studio_ui::SyncVolumeCaches(objects, root, &log) != 0U || !log.empty())
        {
            return 6;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}

int main()
{
    try
    {
        return Run();
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "%s\n", exception.what());
        return 99;
    }
}
