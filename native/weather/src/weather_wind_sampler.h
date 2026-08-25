#pragma once

#include "weather_native.h"
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

namespace godot {

class WeatherWindSampler : public RefCounted {
	GDCLASS(WeatherWindSampler, RefCounted)

protected:
	static void _bind_methods();

public:
	PackedFloat32Array get_global_wind_rgba(Object *weather, int layer) const;
};

} // namespace godot
