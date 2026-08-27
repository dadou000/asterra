#pragma once

#include "geodesic_voronoi_grid.h"
#include "spherical_latlon_sampler.h"
#include "voronoi_dry_core.h"
#include "voronoi_dry_hydrostatic.h"
#include "voronoi_moist_thermodynamics.h"
#include "voronoi_surface_exchange.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

// Runtime-facing Godot bridge for the replacement 30-level atmosphere.
// Dynamics live entirely on the geodesic Voronoi C-grid. Optional moisture is
// carried by the same conservative tracer transport and locally phase-adjusted
// after each accepted dry-core step. Optional prescribed surface water/sensible
// fluxes use the actual accepted atmosphere timestep and are part of the same
// rollback transaction. The 1024x512 products are presentation-only resamples.
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
	asterra::weather::VoronoiMoistThermodynamics::AdjustmentDiagnostics last_moist_adjustment_;
	asterra::weather::VoronoiSurfaceExchange::SurfaceState surface_exchange_state_;
	asterra::weather::VoronoiSurfaceExchange::Diagnostics last_surface_exchange_;
	std::vector<double> evaporation_flux_kg_m2_s_;
	std::vector<double> sensible_heat_flux_w_m2_;
	std::vector<int> display_cell_lookup_;
	std::vector<double> surface_height_m_;

	double initial_dry_mass_kg_ = 0.0;
	double initial_theta_mass_kg_k_ = 0.0;
	double initial_dry_energy_j_ = 0.0;
	double initial_absolute_aam_kg_m2_s_ = 0.0;
	double initial_total_water_kg_ = 0.0;
	double initial_surface_system_water_kg_ = 0.0;
	double initial_surface_system_energy_j_ = 0.0;
	double simulation_seconds_ = 0.0;
	int64_t rejected_steps_total_ = 0;
	int frequency_ = 0;
	bool moisture_enabled_ = false;
	bool surface_exchange_enabled_ = false;

	static asterra::weather::Vec3d to_vec3d(const Vector3 &v);
	int nearest_cell_hill_climb(const asterra::weather::Vec3d &direction, int seed) const;
	void rebuild_display_lookup();
	void reset_budget_baseline();
	void reset_moisture_baseline();
	void reset_surface_exchange_baseline();
	double current_total_water_kg() const;
	double current_surface_system_water_kg() const;
	double current_surface_system_energy_j() const;
	void refresh_state_extrema();
	void on_static_surface_changed();
	void clear_surface_exchange_state();
	void clear_moisture_state();
	bool ready() const { return bool(grid_) && bool(dynamics_); }

protected:
	static void _bind_methods();

public:
	WeatherDryCoreNative() = default;
	~WeatherDryCoreNative() override = default;

	void initialize(int p_frequency = 32,
		double p_surface_pressure_pa = 110000.0,
		double p_temperature_k = 288.0);

	// Runtime transaction order:
	//   conservative dry dynamics/remap
	//   -> prescribed surface exchange using accepted_dt_s
	//   -> reversible saturation adjustment.
	// Any source failure restores both atmospheric and surface states to the
	// complete pre-step snapshot.
	double step(double requested_dt_s, double target_cfl = 0.28);
	void reset_isothermal(double surface_pressure_pa = 110000.0,
		double temperature_k = 288.0);
	void reset_terrain_balanced_isothermal(
		double reference_surface_pressure_pa = 110000.0,
		double temperature_k = 288.0);

	// Moisture uses tracer slots 0/1/2 = vapor/cloud-liquid/cloud-ice. Uniform RH
	// initialization leaves dry mass and wind unchanged. Optional immediate phase
	// adjustment is useful when intentionally initializing RH > 1.
	bool initialize_moisture(double relative_humidity = 0.65,
		bool perform_saturation_adjustment = true);
	void disable_moisture(bool clear_water = true);
	bool saturation_adjust_moisture();
	bool is_moisture_enabled() const { return moisture_enabled_; }

	// Conservative surface-reservoir bridge for the replacement core. Moisture
	// must be enabled first. Water/energy are per native Voronoi cell area.
	// set_surface_fluxes_cells stores rates; step() applies them using the actual
	// accepted atmosphere dt, never the requested dt.
	bool initialize_surface_exchange(double water_kg_m2 = 20.0,
		double energy_j_m2 = 0.0);
	void disable_surface_exchange(bool clear_reservoir = true);
	bool set_surface_fluxes_cells(const PackedFloat32Array &evaporation_kg_m2_s,
		const PackedFloat32Array &sensible_heat_w_m2);
	void clear_surface_fluxes();
	bool is_surface_exchange_enabled() const { return surface_exchange_enabled_; }
	PackedFloat32Array get_surface_water_cells() const;
	PackedFloat32Array get_surface_energy_cells() const;

	// Static atmospheric lower-boundary geometry. These calls do not alter the
	// atmospheric prognostic state; use reset_terrain_balanced_isothermal() after
	// loading a new map when a zero-wind balanced initial condition is desired.
	bool set_surface_height_cells(const PackedFloat32Array &height_m);
	bool set_surface_height_map(const PackedFloat32Array &height_m,
		int width, int height);
	void clear_surface_height();
	PackedFloat32Array get_surface_height_cells() const;

	// Smooth multiplicative column-mass perturbation. All conservative tracer
	// masses receive the same factor as dry mass and theta mass, preserving every
	// tracer mixing ratio at the instant the pressure anomaly is inserted.
	bool add_pressure_perturbation(const Vector3 &center_direction,
		double fractional_amplitude, double angular_radius_rad);

	// Presentation resample for one model level, row-major RGBA:
	//   R = diagnosed temperature [K]
	//   G = horizontal speed estimate [m/s]
	//   B = diagnosed surface pressure [Pa]
	//   A = selected-layer dry mass [kg/m2]
	PackedFloat32Array get_global_dry_rgba(int layer = 0) const;

	// Moist presentation product, row-major RGBA:
	//   R = water-vapor mixing ratio [kg/kg dry]
	//   G = cloud-liquid mixing ratio [kg/kg dry]
	//   B = cloud-ice mixing ratio [kg/kg dry]
	//   A = relative humidity [fraction]
	PackedFloat32Array get_global_moist_rgba(int layer = 0) const;

	PackedFloat32Array get_runtime_diagnostics() const;
	PackedFloat64Array get_global_budget_diagnostics() const;

	// [enabled, initial_total_water_kg, current_total_water_kg,
	//  relative_total_water_drift, last_adjust_relative_water_error,
	//  last_adjust_max_cell_water_error, last_adjust_max_enthalpy_error_Jkg,
	//  last_max_RH_before, last_max_RH_after, last_max_abs_dT_K,
	//  last_condensed_kg, last_evaporated_kg, last_min_T_K, last_max_T_K,
	//  last_saturated_cell_count]
	PackedFloat64Array get_moisture_diagnostics() const;

	// [enabled, initial_system_water_kg, current_system_water_kg,
	//  relative_system_water_drift, initial_system_thermo_energy_j,
	//  current_system_thermo_energy_j, relative_system_energy_drift,
	//  last_requested_dt_s, last_water_error, last_energy_error,
	//  last_evaporated_kg, last_dew_kg, last_sensible_J, last_latent_J,
	//  last_min_surface_water_kg_m2, last_min_bottom_T_K, last_max_bottom_T_K]
	PackedFloat64Array get_surface_exchange_diagnostics() const;

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
