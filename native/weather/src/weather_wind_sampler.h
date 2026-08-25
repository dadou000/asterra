#pragma once

#include "weather_native.h"
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

class WeatherWindSampler : public RefCounted {
	GDCLASS(WeatherWindSampler, RefCounted)

protected:
	static void _bind_methods();

public:
	PackedFloat32Array get_global_wind_rgba(Object *weather, int layer) const;
	Vector3 sample_wind(Object *weather, const Vector3 &direction, float altitude_m) const;
};

} // namespace godot
