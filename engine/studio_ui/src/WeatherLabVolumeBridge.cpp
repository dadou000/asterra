#include <orbit/studio_ui/WeatherLabVolumeBridge.hpp>

#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <string_view>
#include <utility>

namespace orbit::studio_ui
{
WeatherLabVolumeSink MakeWeatherLabVolumeSink(
    std::function<const scene::ObjectStore*()> objects)
{
    return [objects = std::move(objects)](
               const std::string& volumeId,
               const weather_lab::CloudVolumeGrid* grid) -> WeatherLabVolumeResult
    {
        WeatherLabVolumeResult result;
        const scene::ObjectStore* store = objects ? objects() : nullptr;
        if (store == nullptr)
        {
            result.error = "no world is open";
            return result;
        }
        const auto id = scene::ObjectId::Parse(volumeId);
        if (!id.has_value())
        {
            result.error = "volume_id is not an object id: " + volumeId;
            return result;
        }
        const auto domain = world_model::ResolveVolumeDomain(*store, *id);
        if (!domain.has_value())
        {
            result.error = "object " + volumeId + " is not a Volume";
            return result;
        }

        result.representationMode = std::string(
            world_model::VolumeRepresentationModeName(domain->representationMode));
        result.representationOk =
            domain->representationMode == world_model::VolumeRepresentationMode::Baked;
        result.currentHalfExtents[0] = domain->halfExtentsMeters.x;
        result.currentHalfExtents[1] = domain->halfExtentsMeters.y;
        result.currentHalfExtents[2] = domain->halfExtentsMeters.z;

        if (grid == nullptr)
        {
            volume_representation::VolumeCaches().Detach(*id);
            return result;
        }

        result.recommendedHalfExtents[0] = grid->sizeX * 0.5;
        result.recommendedHalfExtents[1] = grid->sizeY * 0.5;
        result.recommendedHalfExtents[2] = grid->sizeZ * 0.5;

        volume_representation::VolumeCacheData cache;
        cache.descriptor.resolutionX = grid->resolutionX;
        cache.descriptor.resolutionY = grid->resolutionY;
        cache.descriptor.resolutionZ = grid->resolutionZ;
        cache.descriptor.fieldMask =
            static_cast<u64>(world_model::VolumeField::Density);
        cache.descriptor.centerMeters = domain->centerMeters;
        cache.descriptor.halfExtentsMeters = domain->halfExtentsMeters;
        cache.density = grid->density;
        volume_representation::FinalizeVolumeCache(cache);
        volume_representation::VolumeCaches().Attach(*id, std::move(cache));
        return result;
    };
}
} // namespace orbit::studio_ui
