#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace godot {

class WeatherNative : public RefCounted {
	GDCLASS(WeatherNative, RefCounted)

public:
	static constexpr int GLOBAL_W = 1024;
	static constexpr int GLOBAL_H = 512;
	static constexpr int LOCAL_W = 192;
	static constexpr int LOCAL_H = 192;
	static constexpr int LAYERS = 30;
	static constexpr int INTERFACES = LAYERS - 1;
	static constexpr int HORIZON_SECTORS = 24;
	static constexpr float PLANET_RADIUS_M = 3500000.0f;
	static constexpr float LOCAL_CELL_M = 2200.0f;

	// 30 non-uniform pressure/sigma levels. The lowest kilometre is densely
	// sampled for boundary-layer physics, while the top reaches ~20.2 km.
	// sigma ~= exp(-z/8 km), preserving the old six anchor levels closely.
	static constexpr std::array<float, LAYERS> SIGMA = {
		0.987578f, 0.969233f, 0.945303f, 0.916219f, 0.882497f, 0.844720f,
		0.803523f, 0.759572f, 0.713552f, 0.666144f, 0.618010f, 0.569783f,
		0.522046f, 0.475328f, 0.430095f, 0.386741f, 0.345591f, 0.308819f,
		0.275960f, 0.246597f, 0.220358f, 0.196912f, 0.175960f, 0.157237f,
		0.140507f, 0.125556f, 0.112197f, 0.100259f, 0.089591f, 0.080058f
	};
	static constexpr std::array<float, LAYERS> APPROX_HEIGHT_M = {
		100.0f, 250.0f, 450.0f, 700.0f, 1000.0f, 1350.0f, 1750.0f, 2200.0f,
		2700.0f, 3250.0f, 3850.0f, 4500.0f, 5200.0f, 5950.0f, 6750.0f, 7600.0f,
		8500.0f, 9400.0f, 10300.0f, 11200.0f, 12100.0f, 13000.0f, 13900.0f, 14800.0f,
		15700.0f, 16600.0f, 17500.0f, 18400.0f, 19300.0f, 20200.0f
	};
	// Pressure-thickness contribution normalized to a mean of one. The sum is
	// exactly ~30, so column products can explicitly rescale to the old six-level
	// optical calibration where required.
	static constexpr std::array<float, LAYERS> DEFAULT_LAYER_WEIGHTS = {
		0.700583f, 0.685757f, 0.859964f, 1.018795f, 1.159806f, 1.281069f,
		1.381213f, 1.459443f, 1.515535f, 1.549812f, 1.563101f, 1.556674f,
		1.532182f, 1.491570f, 1.437001f, 1.370766f, 1.264000f, 1.129507f,
		1.009324f, 0.901929f, 0.805962f, 0.720205f, 0.643574f, 0.575096f,
		0.513904f, 0.459223f, 0.410361f, 0.366697f, 0.327680f, 0.309267f
	};

	struct Atmosphere {
		int width = 0;
		int height = 0;
		int cells = 0;
		std::vector<float> theta, q, u, v, liquid, ice, pressure;
		std::vector<float> ntheta, nq, nu, nv, nliquid, nice, npressure;
		std::vector<float> precip;
		std::vector<float> nprecip;
		std::vector<float> mass_flux;
		std::vector<float> convective_activation;
		std::vector<float> divergence;
		std::vector<float> vorticity;
		std::vector<float> potential_vorticity;
		std::vector<float> shear;

		void resize(int p_width, int p_height);
		int layer_offset(int layer) const { return layer * cells; }
		int interface_offset(int interface_index) const { return interface_index * cells; }
	};

	struct SurfaceState {
		int width = 0;
		int height = 0;
		int cells = 0;
		std::vector<float> elevation_m;
		std::vector<float> water_fraction;
		std::vector<float> soil_moisture;
		std::vector<float> base_albedo;
		std::vector<float> dir_x, dir_y, dir_z;
		std::vector<float> normal_x, normal_y, normal_z;
		std::vector<float> temperature_k;
		std::vector<float> subsurface_temperature_k;
		std::vector<float> snow_swe_kg_m2;
		std::vector<float> snow_age_s;
		std::vector<float> snow_wetness;
		std::vector<float> albedo;
		std::vector<float> absorbed_solar_w_m2;
		std::vector<float> sensible_flux_w_m2;
		std::vector<float> latent_flux_w_m2;
		std::vector<float> ground_flux_w_m2;
		std::vector<float> horizon_tan;
		std::vector<float> sky_view_factor;
		std::vector<float> terrain_sun_visibility;

		void resize(int p_width, int p_height);
	};

private:
	enum TuningIndex {
		CIRCULATION,
		TEMPERATURE,
		HUMIDITY,
		CLOUD_MICROPHYSICS,
		CONVECTION,
		PRECIPITATION,
		TUNING_COUNT,
	};

	Atmosphere global_atm;
	Atmosphere local_atm;
	SurfaceState global_surface;
	SurfaceState local_surface;
	std::array<float, TUNING_COUNT> tuning_weights = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	std::array<float, LAYERS> layer_weights = DEFAULT_LAYER_WEIGHTS;
	std::array<int, 2> tropical_core_cell = {-1, -1};
	std::array<float, 2> tropical_core_age_s = {0.0f, 0.0f};
	std::vector<float> tropical_genesis_activity;
	uint64_t seed = 1;
	double global_simulation_seconds = 0.0;
	Vector3 local_center = Vector3(0, 1, 0);
	Vector3 local_east = Vector3(1, 0, 0);
	Vector3 local_north = Vector3(0, 0, 1);
	Vector3 solar_direction_body = Vector3(-1, 0, 0);
	float solar_irradiance_w_m2 = 1420.0f;
	float helion_angular_radius_rad = 0.00465f;
	bool local_initialized = false;
	bool surface_fields_ready = false;
	bool local_surface_fields_ready = false;

	static int wrap_x(int x, int w);
	static int clamp_y(int y, int h);
	static int spherical_cell(int x, int y, int w, int h);
	static int global_cell(int x, int y);
	static int local_cell(int x, int y);
	float sample_global_layer(const std::vector<float> &field, int layer, float x, float y) const;
	float sample_global_scalar(const std::vector<float> &field, float x, float y) const;
	float sample_global_surface_scalar(const std::vector<float> &field, const Vector3 &dir) const;
	void swap_state(Atmosphere &a);
	void update_local_basis(const Vector3 &center_dir);
	void initialize_global();
	void initialize_local();
	void initialize_global_surface_geometry();
	void update_global_surface_normals();
	void initialize_global_surface_state();
	void initialize_local_surface();
	void update_local_surface_geometry();
	void update_local_surface_horizon();
	void horizontal_pass(Atmosphere &a, bool is_global, float dt);
	void surface_pass(Atmosphere &a, SurfaceState &surface, bool is_global, float dt);
	void vertical_pass(Atmosphere &a, bool is_global, float dt);
	void diagnose(Atmosphere &a, bool is_global);
	void center_global_pressure(std::vector<float> &pressure);
	void filter_global_poles(Atmosphere &a);
	void nudge_local_boundaries(float dt);
	void rotate_global_wind_to_local(const Vector3 &dir, float &u, float &v) const;
	void global_state_at_dir_layer(const Vector3 &dir, int layer,
		float &theta, float &q, float &u, float &v, float &liquid,
		float &ice, float &pressure) const;
	static float actual_temperature(float theta, int layer);
	static float qsat_scalar(float temperature_k, float pressure_pa);
	static float clamp01(float x);
	static int tuning_index(const StringName &name);

	void horizontal_pass_original(Atmosphere &a, bool is_global, float dt);
	void vertical_pass_original(Atmosphere &a, bool is_global, float dt);
	void step_global_original(float dt);
	void step_local_original(float dt);
	PackedFloat32Array get_global_weather_rgba_original() const;
	PackedFloat32Array get_local_weather_rgba_original() const;

protected:
	static void _bind_methods();
	static void _bind_methods_original();

public:
	WeatherNative();
	~WeatherNative() override = default;

	void initialize(int64_t p_seed);
	void step_global(float dt);
	void set_local_center(const Vector3 &center_dir);
	void step_local(float dt);
	void reset_local_from_global();
	void set_global_surface_fields(const PackedFloat32Array &fields);
	void set_local_surface_fields(const PackedFloat32Array &fields);
	void set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2,
		float angular_radius_rad);

	PackedFloat32Array get_global_weather_rgba() const;
	PackedFloat32Array get_local_weather_rgba() const;
	PackedFloat32Array get_global_diagnostics_rgba() const;
	PackedFloat32Array get_local_diagnostics_rgba() const;
	PackedFloat32Array get_global_convective_rgba() const;
	PackedFloat32Array get_local_convective_rgba() const;
	PackedFloat32Array get_tropical_core_diagnostics() const;
	PackedFloat32Array get_global_surface_rgba() const;
	PackedFloat32Array get_local_surface_rgba() const;
	PackedFloat32Array get_global_products_rgba() const;
	PackedFloat32Array get_local_products_rgba() const;
	void set_tuning_weight(const StringName &name, float value);
	float get_tuning_weight(const StringName &name) const;
	void reset_tuning_weights();
	void set_layer_weight(int layer, float value);
	PackedFloat32Array get_layer_weights() const;

	int get_layer_count() const { return LAYERS; }
	int get_global_width() const { return GLOBAL_W; }
	int get_global_height() const { return GLOBAL_H; }
	float get_layer_height_m(int layer) const {
		return layer >= 0 && layer < LAYERS ? APPROX_HEIGHT_M[layer] : 0.0f;
	}
	Vector3 get_local_center() const { return local_center; }
	Vector3 get_local_east() const { return local_east; }
	Vector3 get_local_north() const { return local_north; }
	float get_local_span_m() const { return LOCAL_CELL_M * float(LOCAL_W); }

	// Native-only read access for zero-copy diagnostics helpers. This is not bound
	// to Godot; WeatherWindSampler packs only the selected layer into a GPU texture.
	const Atmosphere &get_global_atmosphere_cpp() const { return global_atm; }
	const Atmosphere &get_local_atmosphere_cpp() const { return local_atm; }
};

} // namespace godot
