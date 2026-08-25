#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
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
	static constexpr float PLANET_RADIUS_M = 1000000.0f;
	static constexpr float LOCAL_CELL_M = 2200.0f;

	// Same sigma discretisation is used by both global and local solvers. Layer 0
	// is the boundary layer and layer 5 is the upper troposphere / lower
	// stratosphere. Approximate standard-atmosphere heights are only metadata;
	// dynamics are carried in terrain-following pressure (sigma) coordinates.
	static constexpr std::array<float, LAYERS> SIGMA = {
		0.95f, 0.82f, 0.68f, 0.52f, 0.36f, 0.20f
	};
	static constexpr std::array<float, LAYERS> APPROX_HEIGHT_M = {
		450.0f, 1700.0f, 3300.0f, 5600.0f, 8500.0f, 12800.0f
	};

private:
	struct Atmosphere {
		int width = 0;
		int height = 0;
		int cells = 0;

		// Prognostic state. Wind is deliberately tangent-space u/v rather than
		// XYZ or angle+magnitude. theta is potential temperature [K], q is
		// specific humidity [kg/kg], condensates are mixing ratios [kg/kg], and
		// pressure is a pressure/geopotential perturbation [Pa-like].
		std::vector<float> theta, q, u, v, liquid, ice, pressure;
		std::vector<float> ntheta, nq, nu, nv, nliquid, nice, npressure;

		// Column/interface state.
		std::vector<float> precip;       // normalised instantaneous precipitation
		std::vector<float> nprecip;
		std::vector<float> mass_flux;    // s^-1, positive upward, 5 interfaces

		// Derived diagnostics. These are scratch/diagnostic fields, not primary
		// state, so vorticity never duplicates information already present in u/v.
		std::vector<float> divergence;
		std::vector<float> vorticity;
		std::vector<float> potential_vorticity;
		std::vector<float> shear;

		void resize(int p_width, int p_height);
		int layer_offset(int layer) const { return layer * cells; }
		int interface_offset(int interface_index) const { return interface_index * cells; }
	};

	Atmosphere global_atm;
	Atmosphere local_atm;
	uint64_t seed = 1;
	Vector3 local_center = Vector3(0, 1, 0);
	Vector3 local_east = Vector3(1, 0, 0);
	Vector3 local_north = Vector3(0, 0, 1);
	bool local_initialized = false;

	static int wrap_x(int x, int w);
	static int clamp_y(int y, int h);
	static int global_cell(int x, int y);
	static int local_cell(int x, int y);

	float sample_global_layer(const std::vector<float> &field, int layer, float x, float y) const;
	void swap_state(Atmosphere &a);
	void update_local_basis(const Vector3 &center_dir);
	void initialize_global();
	void initialize_local();
	void horizontal_pass(Atmosphere &a, bool is_global, float dt);
	void vertical_pass(Atmosphere &a, bool is_global, float dt);
	void diagnose(Atmosphere &a, bool is_global);
	void nudge_local_boundaries(float dt);
	void global_state_at_dir_layer(const Vector3 &dir, int layer,
		float &theta, float &q, float &u, float &v, float &liquid,
		float &ice, float &pressure) const;

	static float actual_temperature(float theta, int layer);
	static float qsat_scalar(float temperature_k, float pressure_pa);
	static float clamp01(float x);

protected:
	static void _bind_methods();

public:
	WeatherNative();
	~WeatherNative() override = default;

	void initialize(int64_t p_seed);
	void step_global(float dt);
	void set_local_center(const Vector3 &center_dir);
	void step_local(float dt);

	// Renderer-compatible aggregate fields:
	// R cloud coverage, G organised-storm signal, B precipitation, A pressure.
	PackedFloat32Array get_global_weather_rgba() const;
	PackedFloat32Array get_local_weather_rgba() const;

	// Map/tuning diagnostics:
	// R signed relative vorticity, G signed divergence, B signed PV proxy,
	// A maximum column vertical wind shear; signed channels are encoded about .5.
	PackedFloat32Array get_global_diagnostics_rgba() const;
	PackedFloat32Array get_local_diagnostics_rgba() const;

	int get_layer_count() const { return LAYERS; }
	Vector3 get_local_center() const { return local_center; }
	Vector3 get_local_east() const { return local_east; }
	Vector3 get_local_north() const { return local_north; }
	float get_local_span_m() const { return LOCAL_CELL_M * float(LOCAL_W); }
};

} // namespace godot
