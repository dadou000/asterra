#include "weather_wind_sampler.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>

namespace godot {

void WeatherWindSampler::_bind_methods() {
	ClassDB::bind_method(
		D_METHOD("get_global_wind_rgba", "weather", "layer"),
		&WeatherWindSampler::get_global_wind_rgba);
}

PackedFloat32Array WeatherWindSampler::get_global_wind_rgba(Object *weather, int layer) const {
	PackedFloat32Array out;
	const WeatherNative *native = Object::cast_to<WeatherNative>(weather);
	if (native == nullptr) return out;

	const WeatherNative::Atmosphere &a = native->get_global_atmosphere_cpp();
	if (a.cells != WeatherNative::GLOBAL_W * WeatherNative::GLOBAL_H) return out;
	const int selected = std::clamp(layer, 0, WeatherNative::LAYERS - 1);
	const int offset = a.layer_offset(selected);
	if (offset < 0 || size_t(offset + a.cells) > a.u.size()
			|| size_t(offset + a.cells) > a.v.size()) {
		return out;
	}

	out.resize(a.cells * 4);
	float *dst = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		const float u = a.u[offset + c];
		const float v = a.v[offset + c];
		const float speed = std::sqrt(u * u + v * v);
		dst[c * 4 + 0] = u;
		dst[c * 4 + 1] = v;
		dst[c * 4 + 2] = speed;
		dst[c * 4 + 3] = 1.0f;
	}
	return out;
}

} // namespace godot
