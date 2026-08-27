#pragma once

#include "geodesic_voronoi_grid.h"
#include "voronoi_dry_hydrostatic.h"

#include <vector>

namespace asterra::weather {

// Conservative horizontal transport foundation for the 30-level dry
// hydrostatic atmosphere.
//
// The prognostic cell variables are layer dry-air mass [kg/m2] and
// mass-weighted potential temperature [kg K/m2]. One globally-oriented normal
// velocity is stored per physical edge and level. A single shared edge mass
// flux transports both quantities, so the two adjacent cells always receive
// exactly equal/opposite transfers. No latitude/pole branch or post-step clamp
// exists in this core.
//
// Momentum is deliberately frozen in this first Phase-4 slice. The next dry
// dynamics step will couple the existing hydrostatic pressure-gradient/TRiSK
// momentum operator to this conservative mass/thermodynamic transport.
class VoronoiDryTransport {
public:
	static constexpr int LEVELS = VoronoiDryHydrostatic::LEVELS;

	struct State {
		std::vector<double> layer_mass_kg_m2;       // [level][cell]
		std::vector<double> theta_mass_kg_k_m2;    // [level][cell]
		std::vector<double> edge_normal_mps;       // [level][edge], positive a -> b
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
		double min_layer_mass_kg_m2 = 0.0;
		double min_potential_temperature_k = 0.0;
		double max_speed_mps = 0.0;
		int rejected_steps = 0;
	};

	VoronoiDryTransport(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0);

	// Creates the same 30-level hydrostatic reference profile used by
	// VoronoiDryHydrostatic, converted to conservative mass-form variables.
	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const;

	double potential_temperature(const State &state, int level, int cell) const;
	double total_dry_mass_kg(const State &state) const;
	double total_theta_mass_kg_k(const State &state) const;

	// Finite-volume outflow Courant number. For first-order donor-cell fluxes,
	// max_courant <= 1 is the positivity bound; production uses <= 0.45.
	double max_courant(const State &state, double dt_s) const;
	double stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const;

	// SSPRK3 with rollback. The accepted dt may be smaller than requested_dt_s
	// due to the CFL controller. A failed stage restores the original state and
	// retries with half dt; accepted states are never clipped into validity.
	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.45, int max_retries = 10) const;

	const GeodesicVoronoiGrid &grid() const { return *grid_; }

private:
	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryHydrostatic hydrostatic_;

	int scalar_index(int level, int cell) const {
		return level * grid_->cell_count() + cell;
	}
	int edge_index(int level, int edge) const {
		return level * grid_->edge_count() + edge;
	}

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	Tendencies compute_tendencies(const State &state) const;
	bool euler_stage(const State &input, State &output, double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s) const;
};

} // namespace asterra::weather
