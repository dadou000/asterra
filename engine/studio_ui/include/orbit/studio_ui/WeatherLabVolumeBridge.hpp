#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_ui/WeatherLabRpc.hpp>

#include <functional>

namespace orbit::studio_ui
{
// Connects the Weather Lab to Studio Volume objects. The sink validates that
// the id names a Volume, converts the density grid into a volume cache (the
// same VolumeCacheData the Volumes panel bakes and imports), attaches it to the
// shared VolumeCaches() registry, and reports the volume's representation mode
// and extents so the caller can tell what still has to be set for the cache to
// draw (mode Baked, extents matching the storm). It never edits the world.
//
// `objects` is called on each use because the active world can be replaced.
[[nodiscard]] WeatherLabVolumeSink MakeWeatherLabVolumeSink(
    std::function<const scene::ObjectStore*()> objects);
} // namespace orbit::studio_ui
