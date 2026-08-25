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
	static constexpr int GLOBAL_W = 256;
	static constexpr int GLOBAL_H = 128;
	static constexpr int LOCAL_W = 192;
	static constexpr int LOCAL_H = 192;
	static constexpr int LAYERS = 6;
	static constexpr int INTERFACES = LAYERS - 1;
	static constexpr int HORIZON_SECTORS = 24;
	static constexpr float PLANET_RADIUS_M = 3500000.0f;
	static constexpr float LOCAL_CELL_M = 2200.0f;

	static constexpr std::array<float, LAYERS> SIGMA = {
		0.95f, 0.82f, 0.68f, 0.52f, 0.36f, 0.20f
	};
	static constexpr std::array<float, LAYERS> APPROX_HEIGHT_M = {
		450.0f, 1700.0f, 3300.0f, 5600.0f, 8500.0f, 12800.0f
	};
	// Pressure-thickness weights, normalized to a mean of one. The six levels
	// represent unequal portions of the atmospheric column, so equal weighting
	// would overstate the thin near-surface level and understate the deep top bin.
	static constexpr std::array<float, LAYERS> DEFAULT_LAYER_WEIGHTS = {
		0.77f, 0.90f, 1.00f, 1.07f, 1.07f, 1.19f
	};

	// Public only so translation-unit-local packing helpers can consume a const
	// Atmosphere without duplicating the type. It is not exposed to Godot.
	struct Atmosphere {
		int width = 0;
		int height = 0;
		int cells = 0;

		// Prognostic layer-major SoA state. Wind is local tangent-space u/v,
		// theta is potential temperature [K], q is specific humidity [kg/kg],
		// condensates are mixing ratios [kg/kg], and pressure is a perturbation.
		std::vector<float> theta, q, u, v, liquid, ice, pressure;
		std::vector<float> ntheta, nq, nu, nv, nliquid, nice, npressure;

		// Column/interface state.
		std::vector<float> precip;
		std::vector<float> nprecip;
		std::vector<float> mass_flux;

		// Capped-convection memory. 0 is a loaded but inhibited column; 1 is an
		// actively released convective column. This lets CAPE accumulate under an
		// elevated warm layer and release only at convergence/dryline/heat triggers
		// instead of turning every humid grid box into permanent cloud.
		std::vector<float> convective_activation;

		// Derived diagnostics, never duplicated as prognostic wind state.
		std::vector<float> divergence;
		std::vector<float> vorticity;
		std::vector<float> potential_vorticity;
		std::vector<float> shear;

		void resize(int p_width, int p_height);
		int layer_offset(int layer) const { return layer * cells; }
		int interface_offset(int interface_index) const { return interface_index * cells; }
	};

	// Persistent lower-boundary energy/water state. Static geography is supplied
	// by the procedural planet once its bake is ready; all temperatures, snow and
	// fluxes then evolve continuously with the atmosphere.
	struct SurfaceState {
		int width = 0;
		int height = 0;
		int cells = 0;

		// Geography / material inputs.
		std::vector<float> elevation_m;
		std::vector<float> water_fraction;
		std::vector<float> soil_moisture;
		std::vector<float> base_albedo;

		// Body-fixed cell position and terrain normal.
		std::vector<float> dir_x, dir_y, dir_z;
		std::vector<float> normal_x, normal_y, normal_z;

		// Prognostic thermal/hydrological state.
		std::vector<float> temperature_k;
		std::vector<float> subsurface_temperature_k;
		std::vector<float> snow_swe_kg_m2;
		std::vector<float> snow_age_s;
		std::vector<float> snow_wetness;

		// Diagnostics.
		std::vector<float> albedo;
		std::vector<float> absorbed_solar_w_m2;
		std::vector<float> sensible_flux_w_m2;
		std::vector<float> latent_flux_w_m2;
		std::vector<float> ground_flux_w_m2;
		// Local terrain horizon, stored as tangent(elevation angle) per azimuth.
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

	// The tuned translation unit wraps a few baseline functions while continuing
	// to include the proven AVX2 core. Keeping explicit declarations here makes
	// that wrapper type-safe without duplicating the rest of the implementation.
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

	// Packed four-float geography per global weather cell:
	// elevation_m, water_fraction, soil_moisture, snow-free base_albedo.
	void set_global_surface_fields(const PackedFloat32Array &fields);
	void set_local_surface_fields(const PackedFloat32Array &fields);
	void set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2,
		float angular_radius_rad);

	PackedFloat32Array get_global_weather_rgba() const;
	PackedFloat32Array get_local_weather_rgba() const;
	PackedFloat32Array get_global_diagnostics_rgba() const;
	PackedFloat32Array get_local_diagnostics_rgba() const;
	// Convective structure: ascent, downdraft, upper ice/anvil, and surface
	// wind speed. Rotation remains signed in the dynamics diagnostics and can be
	// collocated with ascent by validators. This keeps severe-weather validation
	// separate from the presentation-oriented storm scalar.
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
	Vector3 get_local_center() const { return local_center; }
	Vector3 get_local_east() const { return local_east; }
	Vector3 get_local_north() const { return local_north; }
	float get_local_span_m() const { return LOCAL_CELL_M * float(LOCAL_W); }
};

} // namespace godot