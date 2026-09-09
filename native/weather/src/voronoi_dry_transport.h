#pragma once

#include "geodesic_voronoi_grid.h"
#include "voronoi_dry_hydrostatic.h"

#include <vector>

namespace asterra::weather {

// Conservative horizontal transport foundation for the 30-level dry
// hydrostatic atmosphere.
//
// The prognostic cell variables are layer dry-air mass [kg/m2], mass-weighted
// potential temperature [kg K/m2], and zero or more passive tracer masses
// [kg/m2]. One globally-oriented normal velocity is stored once per physical
// edge and level. A single shared donor dry-mass flux transports theta and every
// tracer, so adjacent cells receive exactly equal/opposite transfers and tracer
// mixing ratios cannot become negative under the CFL/SSPRK positivity gate.
class VoronoiDryTransport {
public:
	static constexpr int LEVELS = VoronoiDryHydrostatic::LEVELS;
	static constexpr int INTERFACES = LEVELS + 1;

	struct State {
		std::vector<double> layer_mass_kg_m2;       // [level][cell]
		std::vector<double> theta_mass_kg_k_m2;    // [level][cell]
		std::vector<double> edge_normal_mps;       // [level][edge], positive a -> b
		std::vector<std::vector<double>> tracer_mass_kg_m2; // [tracer][level][cell]
	};

	struct HydroDiagnostics {
		std::vector<double> surface_pressure_pa;      // [cell]
		std::vector<double> interface_pressure_pa;    // [interface][cell], 0=surface
		std::vector<double> layer_pressure_pa;        // [level][cell]
		std::vector<double> potential_temperature_k;  // [level][cell]
		std::vector<double> temperature_k;            // [level][cell]
		std::vector<double> interface_geopotential;    // [interface][cell], m2/s2
		std::vector<double> layer_geopotential;        // [level][cell], m2/s2
	};

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_courant = 0.0;
		double dry_mass_before_kg = 0.0;
		double dry_mass_after_kg = 0.0;
		double relative_dry_mass_error = 0.0;
		double theta_mass_before_kg_k = 0.0;
		double theta_mass_after_kg_k = 0.0;
		double relative_theta_mass_error = 0.0;
		double max_relative_tracer_mass_error = 0.0;
		double min_layer_mass_kg_m2 = 0.0;
		double min_potential_temperature_k = 0.0;
		double max_speed_mps = 0.0;
		int rejected_steps = 0;
	};

	VoronoiDryTransport(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0,
		double top_pressure_pa = 7500.0);

	// Static lower-boundary geometry. Height is converted to geopotential with
	// the core's constant gravity. Negative terrain is allowed; all values must
	// remain finite. The default is a zero-height spherical surface.
	void set_surface_height_m(const std::vector<double> &height_m);
	void set_surface_geopotential_m2_s2(const std::vector<double> &geopotential);
	const std::vector<double> &surface_geopotential_m2_s2() const {
		return surface_geopotential_m2_s2_;
	}

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const;
	State make_isothermal_terrain_balanced_reference(
		double reference_surface_pressure_pa,
		double temperature_k) const;

	HydroDiagnostics diagnose_hydrostatic(const State &state) const;
	std::vector<double> pressure_gradient_acceleration(const State &state,
		const HydroDiagnostics &diagnostics) const;

	double potential_temperature(const State &state, int level, int cell) const;
	double total_dry_mass_kg(const State &state) const;
	double total_theta_mass_kg_k(const State &state) const;
	double total_tracer_mass_kg(const State &state, int tracer) const;

	double max_courant(const State &state, double dt_s) const;
	double stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const;

	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.45, int max_retries = 10) const;

	const GeodesicVoronoiGrid &grid() const { return *grid_; }
	double top_pressure_pa() const { return top_pressure_pa_; }
	double gravity_mps2() const { return gravity_mps2_; }

private:
	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
		std::vector<std::vector<double>> tracer_mass_dt;
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryHydrostatic hydrostatic_;
	double gravity_mps2_ = 9.80665;
	double top_pressure_pa_ = 7500.0;
	std::vector<double> surface_geopotential_m2_s2_;

	int scalar_index(int level, int cell) const {
		return level * grid_->cell_count() + cell;
	}
	int interface_index(int interface_level, int cell) const {
		return interface_level * grid_->cell_count() + cell;
	}
	int edge_index(int level, int edge) const {
		return level * grid_->edge_count() + edge;
	}

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	State make_isothermal_with_surface_pressure(
		const std::vector<double> &surface_pressure_pa,
		double temperature_k) const;
	Tendencies compute_tendencies(const State &state) const;
	bool euler_stage(const State &input, State &output, double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s) const;
};

} // namespace asterra::weather
