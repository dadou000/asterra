#include "weather_wind_sampler.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace godot {

void WeatherWindSampler::_bind_methods() {
	ClassDB::bind_method(
		D_METHOD("get_global_wind_rgba", "weather", "layer"),
		&WeatherWindSampler::get_global_wind_rgba);
	ClassDB::bind_method(
		D_METHOD("sample_wind", "weather", "direction", "altitude_m"),
		&WeatherWindSampler::sample_wind);
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

Vector3 WeatherWindSampler::sample_wind(Object *weather, const Vector3 &direction,
		float altitude_m) const {
	const WeatherNative *native = Object::cast_to<WeatherNative>(weather);
	if (native == nullptr || direction.length_squared() < 1e-10f) return Vector3();
	const WeatherNative::Atmosphere &a = native->get_global_atmosphere_cpp();
	if (a.cells != WeatherNative::GLOBAL_W * WeatherNative::GLOBAL_H) return Vector3();

	const Vector3 n = direction.normalized();
	constexpr float PI = std::numbers::pi_v<float>;
	constexpr float TAU = PI * 2.0f;
	float lon = std::atan2(n.z, n.x);
	if (lon < 0.0f) lon += TAU;
	const float lat = std::asin(std::clamp(n.y, -1.0f, 1.0f));
	const float xf = lon / TAU * float(WeatherNative::GLOBAL_W) - 0.5f;
	const float yf = (PI * 0.5f - lat) / PI * float(WeatherNative::GLOBAL_H) - 0.5f;
	const int x0_raw = int(std::floor(xf));
	const int y0 = std::clamp(int(std::floor(yf)), 0, WeatherNative::GLOBAL_H - 1);
	const int y1 = std::min(y0 + 1, WeatherNative::GLOBAL_H - 1);
	const int x0 = (x0_raw % WeatherNative::GLOBAL_W + WeatherNative::GLOBAL_W) % WeatherNative::GLOBAL_W;
	const int x1 = (x0 + 1) % WeatherNative::GLOBAL_W;
	const float tx = xf - std::floor(xf);
	const float ty = std::clamp(yf - std::floor(yf), 0.0f, 1.0f);

	auto bilinear = [&](const std::vector<float> &field, int layer) -> float {
		const int off = a.layer_offset(layer);
		const float f00 = field[off + x0 + y0 * WeatherNative::GLOBAL_W];
		const float f10 = field[off + x1 + y0 * WeatherNative::GLOBAL_W];
		const float f01 = field[off + x0 + y1 * WeatherNative::GLOBAL_W];
		const float f11 = field[off + x1 + y1 * WeatherNative::GLOBAL_W];
		const float a0 = f00 + (f10 - f00) * tx;
		const float a1 = f01 + (f11 - f01) * tx;
		return a0 + (a1 - a0) * ty;
	};

	const float h = std::max(altitude_m, 0.0f);
	int lo = 0;
	int hi = 0;
	float ht = 0.0f;
	if (h <= WeatherNative::APPROX_HEIGHT_M.front()) {
		lo = hi = 0;
	} else if (h >= WeatherNative::APPROX_HEIGHT_M.back()) {
		lo = hi = WeatherNative::LAYERS - 1;
	} else {
		for (int layer = 0; layer < WeatherNative::LAYERS - 1; ++layer) {
			const float h0 = WeatherNative::APPROX_HEIGHT_M[layer];
			const float h1 = WeatherNative::APPROX_HEIGHT_M[layer + 1];
			if (h >= h0 && h <= h1) {
				lo = layer;
				hi = layer + 1;
				ht = (h - h0) / std::max(h1 - h0, 1.0f);
				break;
			}
		}
	}

	const float u0 = bilinear(a.u, lo);
	const float v0 = bilinear(a.v, lo);
	const float u1 = bilinear(a.u, hi);
	const float v1 = bilinear(a.v, hi);
	const float u = u0 + (u1 - u0) * ht;
	const float v = v0 + (v1 - v0) * ht;

	Vector3 east(-n.z, 0.0f, n.x);
	if (east.length_squared() < 1e-10f) east = Vector3(1, 0, 0);
	else east.normalize();
	const Vector3 north = east.cross(n).normalized();
	return east * u + north * v;
}

} // namespace godot
