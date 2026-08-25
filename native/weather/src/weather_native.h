#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <vector>
#include <cstdint>

namespace godot {

class WeatherNative : public RefCounted {
	GDCLASS(WeatherNative, RefCounted)

public:
	static constexpr int GLOBAL_W = 256;
	static constexpr int GLOBAL_H = 128;
	static constexpr int LOCAL_W = 192;
	static constexpr int LOCAL_H = 192;
	static constexpr float PLANET_RADIUS_M = 1000000.0f;
	static constexpr float LOCAL_CELL_M = 2200.0f;

private:
	struct Fields {
		std::vector<float> temp, pressure, humidity, u, v, cloud, cape, vort, precip;
		std::vector<float> nt, np, nq, nu, nv, nc, ncap, nvor, nrain;
		void resize(size_t n);
	};

	Fields g;
	Fields l;
	uint64_t seed = 1;
	Vector3 local_center = Vector3(0, 1, 0);
	Vector3 local_east = Vector3(1, 0, 0);
	Vector3 local_north = Vector3(0, 0, 1);
	bool local_initialized = false;

	static int wrap_x(int x, int w);
	static int clamp_y(int y, int h);
	static int gi(int x, int y);
	static int li(int x, int y);
	float sample_global(const std::vector<float> &a, float x, float y) const;
	float sample_local(const std::vector<float> &a, float x, float y) const;
	void swap_global();
	void swap_local();
	void update_local_basis(const Vector3 &center_dir);
	void initialize_global();
	void initialize_local();
	void global_state_at_dir(const Vector3 &dir, float &cloud, float &cape, float &rain, float &pressure,
		float &temp, float &humidity, float &u, float &v) const;

protected:
	static void _bind_methods();

public:
	WeatherNative();
	~WeatherNative() override = default;

	void initialize(int64_t p_seed);
	void step_global(float dt);
	void set_local_center(const Vector3 &center_dir);
	void step_local(float dt);
	PackedFloat32Array get_global_weather_rgba() const;
	PackedFloat32Array get_local_weather_rgba() const;
	Vector3 get_local_center() const { return local_center; }
	Vector3 get_local_east() const { return local_east; }
	Vector3 get_local_north() const { return local_north; }
	float get_local_span_m() const { return LOCAL_CELL_M * float(LOCAL_W); }
};

} // namespace godot
