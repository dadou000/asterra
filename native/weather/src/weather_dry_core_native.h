#pragma once

#include "geodesic_voronoi_grid.h"
#include "spherical_latlon_sampler.h"
#include "voronoi_dry_core.h"
#include "voronoi_dry_hydrostatic.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

// Runtime-facing Godot bridge for the replacement 30-level dry atmosphere.
// The physical state lives entirely on the geodesic Voronoi C-grid; the
// 1024x512 product is a presentation-only resample and never feeds back into
// dynamics. Every accepted runtime step includes conservative pressure-
// coordinate restoration through VoronoiDryCore.
class WeatherDryCoreNative : public RefCounted {
	GDCLASS(WeatherDryCoreNative, RefCounted)

public:
	static constexpr int DISPLAY_W = 1024;
	static constexpr int DISPLAY_H = 512;
	static constexpr double PLANET_RADIUS_M = 3500000.0;
	static constexpr double GRAVITY_MPS2 = 9.80665;
	static constexpr double ROTATION_PERIOD_S = 11.5 * 3600.0;
	static constexpr double ROTATION_RATE_RAD_S =
		2.0 * 3.141592653589793238462643383279502884 / ROTATION_PERIOD_S;
	static constexpr double TOP_PRESSURE_PA = 7500.0;

private:
	std::unique_ptr<asterra::weather::GeodesicVoronoiGrid> grid_;
	std::unique_ptr<asterra::weather::VoronoiDryCore> dynamics_;
	asterra::weather::VoronoiDryCore::State state_;
	asterra::weather::VoronoiDryCore::StepDiagnostics last_step_;
	std::vector<int> display_cell_lookup_;
	std::vector<double> surface_height_m_;

	double initial_dry_mass_kg_ = 0.0;
	double initial_theta_mass_kg_k_ = 0.0;
	double initial_dry_energy_j_ = 0.0;
	double initial_absolute_aam_kg_m2_s_ = 0.0;
	double simulation_seconds_ = 0.0;
	int64_t rejected_steps_total_ = 0;
	int frequency_ = 0;

	static asterra::weather::Vec3d to_vec3d(const Vector3 &v);
	int nearest_cell_hill_climb(const asterra::weather::Vec3d &direction, int seed) const;
	void rebuild_display_lookup();
	void reset_budget_baseline();
	void refresh_state_extrema();
	void on_static_surface_changed();
	bool ready() const { return bool(grid_) && bool(dynamics_); }

protected:
	static void _bind_methods();

public:
	WeatherDryCoreNative() = default;
	~WeatherDryCoreNative() override = default;

	// F32 is the interactive bring-up default (~10k horizontal cells, 30 levels).
	// Higher frequencies can be selected by the caller as profiling permits.
	void initialize(int p_frequency = 32,
		double p_surface_pressure_pa = 110000.0,
		double p_temperature_k = 288.0);

	double step(double requested_dt_s, double target_cfl = 0.28);
	void reset_isothermal(double surface_pressure_pa = 110000.0,
		double temperature_k = 288.0);
	void reset_terrain_balanced_isothermal(
		double reference_surface_pressure_pa = 110000.0,
		double temperature_k = 288.0);

	// Static atmospheric lower-boundary geometry. These calls do not alter
	// atmospheric mass or theta; they only replace the surface geopotential used
	// by hydrostatics. Use reset_terrain_balanced_isothermal() after loading a new
	// terrain map when a zero-wind balanced initial condition is desired.
	//
	// set_surface_height_cells expects exactly get_cell_count() samples in native
	// Voronoi cell order. set_surface_height_map accepts a global equirectangular
	// raster using the same convention as the weather display: row-major,
	// west->east, north->south, pixel-centred and longitude-periodic.
	bool set_surface_height_cells(const PackedFloat32Array &height_m);
	bool set_surface_height_map(const PackedFloat32Array &height_m,
		int width, int height);
	void clear_surface_height();
	PackedFloat32Array get_surface_height_cells() const;

	// Add a smooth multiplicative column-mass perturbation. Layer mass and theta
	// mass receive the same factor, so potential temperature is unchanged at the
	// instant of insertion. The perturbation therefore enters dynamics through
	// physically diagnosed pressure/geopotential rather than a free pressure field.
	bool add_pressure_perturbation(const Vector3 &center_direction,
		double fractional_amplitude, double angular_radius_rad);

	// Presentation resample for one model level, row-major RGBA:
	//   R = diagnosed temperature [K]
	//   G = horizontal speed estimate [m/s]
	//   B = diagnosed surface pressure [Pa]
	//   A = selected-layer dry mass [kg/m2]
	PackedFloat32Array get_global_dry_rgba(int layer = 0) const;

	// Existing fields remain stable at indices 0..14:
	// [simulation_s, requested_dt_s, accepted_dt_s, max_courant,
	//  rejected_steps_total, relative_dry_mass_drift, relative_theta_mass_drift,
	//  min_layer_mass_kg_m2, min_theta_k, max_speed_mps,
	//  max_pressure_accel_mps2, cell_count, edge_count, level_count, top_pressure_pa,
	//  max_coordinate_fraction_error, max_coordinate_column_mass_error,
	//  max_coordinate_column_theta_error, max_coordinate_edge_momentum_error,
	//  coordinate_remap_applied]
	PackedFloat32Array get_runtime_diagnostics() const;

	// Expensive, opt-in double-precision global budgets. This reconstructs cell
	// winds across all 30 levels, so it is intended for debug/validation sampling
	// rather than every render frame:
	// [dry_mass_kg, theta_mass_kg_k, total_energy_j, relative_energy_drift,
	//  relative_axial_aam_kg_m2_s, absolute_axial_aam_kg_m2_s,
	//  relative_absolute_aam_drift, max_coordinate_fraction_error,
	//  initial_energy_j, initial_absolute_aam_kg_m2_s]
	PackedFloat64Array get_global_budget_diagnostics() const;

	int get_frequency() const { return frequency_; }
	int get_cell_count() const { return grid_ ? grid_->cell_count() : 0; }
	int get_edge_count() const { return grid_ ? grid_->edge_count() : 0; }
	int get_layer_count() const { return asterra::weather::VoronoiDryCore::LEVELS; }
	int get_display_width() const { return DISPLAY_W; }
	int get_display_height() const { return DISPLAY_H; }
	double get_simulation_seconds() const { return simulation_seconds_; }
	double get_top_pressure_pa() const { return TOP_PRESSURE_PA; }
	double get_layer_height_m(int layer) const {
		return layer >= 0 && layer < asterra::weather::VoronoiDryHydrostatic::LEVELS
			? asterra::weather::VoronoiDryHydrostatic::NOMINAL_HEIGHT_M[static_cast<size_t>(layer)]
			: 0.0;
	}
};

} // namespace godot
