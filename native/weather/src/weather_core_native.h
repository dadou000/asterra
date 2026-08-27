#pragma once

#include "geodesic_voronoi_grid.h"
#include "voronoi_shallow_water.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

// Runtime-facing bridge for the replacement spherical C-grid core.
//
// This class deliberately exposes only conservative-core state and diagnostics;
// legacy humidity/cloud/severe tuning does not feed back into it. The existing
// 1024x512 globe texture can consume get_global_core_rgba() while the physical
// state remains on the pole-free Voronoi mesh.
class WeatherCoreNative : public RefCounted {
	GDCLASS(WeatherCoreNative, RefCounted)

public:
	static constexpr int DISPLAY_W = 1024;
	static constexpr int DISPLAY_H = 512;
	static constexpr double PLANET_RADIUS_M = 3500000.0;
	static constexpr double GRAVITY_MPS2 = 9.80665;
	static constexpr double ROTATION_PERIOD_S = 11.5 * 3600.0;
	static constexpr double DEFAULT_ROTATION_RATE_RAD_S =
		2.0 * 3.141592653589793238462643383279502884 / ROTATION_PERIOD_S;

private:
	std::unique_ptr<asterra::weather::GeodesicVoronoiGrid> grid_;
	std::unique_ptr<asterra::weather::VoronoiShallowWater> solver_;
	asterra::weather::VoronoiShallowWater::State state_;
	asterra::weather::VoronoiShallowWater::StepDiagnostics last_step_;
	std::vector<int> display_cell_lookup_;

	asterra::weather::Vec3d rotation_axis_{0.0, 1.0, 0.0};
	double rotation_rate_rad_s_ = DEFAULT_ROTATION_RATE_RAD_S;
	double initial_mass_m3_ = 0.0;
	double initial_energy_ = 0.0;
	double simulation_seconds_ = 0.0;
	int64_t rejected_steps_total_ = 0;
	int frequency_ = 0;

	static asterra::weather::Vec3d to_vec3d(const Vector3 &v);
	static Vector3 to_vector3(const asterra::weather::Vec3d &v);
	int nearest_cell_hill_climb(const asterra::weather::Vec3d &direction, int seed) const;
	void rebuild_display_lookup();
	void reset_budget_baseline();
	bool ready() const { return bool(grid_) && bool(solver_); }

protected:
	static void _bind_methods();

public:
	WeatherCoreNative() = default;
	~WeatherCoreNative() override = default;

	// Build the replacement global core. Frequency is the icosahedral subdivision
	// frequency: cell_count = 10*f^2+2. F64 is ~40k cells and is a practical
	// interactive bring-up setting; higher production resolutions can follow once
	// the 30-level dry atmosphere replaces this shallow-water runtime harness.
	void initialize(int p_frequency = 64, double p_base_depth_m = 5000.0);

	// Advance by up to requested_dt_s. The solver may accept a smaller step after
	// CFL/positivity rollback. Returns the accepted physical timestep, or 0 on
	// unrecoverable rejection while leaving the prior state intact.
	double step(double requested_dt_s, double target_cfl = 0.30);

	// Reset to the balanced Williamson-TC2-style solid-body state used by the
	// native regression suite. flow_rate_fraction is angular flow / planet omega.
	void reset_balanced_flow(double base_depth_m = 5000.0,
		double flow_rate_fraction = 1.0 / 12.0);
	void reset_rest(double depth_m = 5000.0);

	// Add a smooth cosine-bell thickness anomaly for visual/runtime wave testing.
	// Returns false rather than clipping if the requested perturbation would make
	// any cell non-positive.
	bool add_height_perturbation(const Vector3 &center_direction,
		double amplitude_m, double angular_radius_rad);

	void set_rotation_axis(const Vector3 &axis);
	Vector3 get_rotation_axis() const { return to_vector3(rotation_axis_); }
	void set_rotation_rate(double rate_rad_s);
	double get_rotation_rate() const { return rotation_rate_rad_s_; }

	// Raw 1024x512 diagnostic texture, row-major RGBA:
	//   R = layer depth [m]
	//   G = reconstructed horizontal speed [m/s]
	//   B = cell-averaged relative vorticity [1/s]
	//   A = 1
	// It is intentionally a presentation resample; these values never feed back
	// into the conservative state.
	PackedFloat32Array get_global_core_rgba() const;

	// Packed diagnostics:
	// [simulation_s, requested_dt_s, accepted_dt_s, max_courant,
	//  rejected_steps_total, relative_mass_drift, relative_energy_drift,
	//  min_depth_m, max_depth_m, max_speed_mps, cell_count, edge_count]
	PackedFloat32Array get_runtime_diagnostics() const;

	int get_frequency() const { return frequency_; }
	int get_cell_count() const { return grid_ ? grid_->cell_count() : 0; }
	int get_edge_count() const { return grid_ ? grid_->edge_count() : 0; }
	int get_display_width() const { return DISPLAY_W; }
	int get_display_height() const { return DISPLAY_H; }
	double get_simulation_seconds() const { return simulation_seconds_; }
};

} // namespace godot
