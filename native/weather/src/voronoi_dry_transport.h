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
// velocity is stored once per physical edge and level. A single shared edge mass
// flux transports both quantities, so the two adjacent cells always receive
// exactly equal/opposite transfers. No latitude/pole branch or post-step clamp
// exists in this core.
//
// Hydrostatic pressure is diagnosed from the prognostic layer masses, not
// evolved as an independent perturbation scalar. Static surface geopotential is
// part of the hydrostatic geometry, allowing terrain pressure forces without
// turning terrain height into a prognostic fluid variable.
class VoronoiDryTransport {
public:
	static constexpr int LEVELS = VoronoiDryHydrostatic::LEVELS;
	static constexpr int INTERFACES = LEVELS + 1;

	struct State {
		std::vector<double> layer_mass_kg_m2;       // [level][cell]
		std::vector<double> theta_mass_kg_k_m2;    // [level][cell]
		std::vector<double> edge_normal_mps;       // [level][edge], positive a -> b
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

	// Build a horizontally uniform isothermal reference using the existing
	// 30-level vertical-spacing shape, but with a fixed pressure top. The resulting
	// layer masses sum exactly to (ps - p_top)/g and are therefore directly
	// prognostic. This method intentionally ignores terrain balance and keeps the
	// requested pressure identical at every cell (useful for forcing tests).
	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const;

	// Terrain-balanced isothermal reference. reference_surface_pressure_pa is the
	// pressure that would occur at Phi_s=0. Each terrain cell receives
	//   ps = p_ref * exp(-Phi_s / (Rd T)),
	// which exactly cancels the generalized-coordinate pressure force for an
	// isothermal hydrostatic atmosphere, apart from floating-point roundoff.
	State make_isothermal_terrain_balanced_reference(
		double reference_surface_pressure_pa,
		double temperature_k) const;

	// Reconstruct p, T and hydrostatic geopotential directly from conservative
	// layer mass + theta mass. Interface pressure is accumulated downward from
	// the fixed model-top pressure: p_lower = p_upper + g * layer_mass.
	HydroDiagnostics diagnose_hydrostatic(const State &state) const;

	// Generalized-coordinate horizontal pressure-gradient acceleration at each
	// edge/level: -grad_s(Phi) - Rd*T*grad_s(ln p).
	std::vector<double> pressure_gradient_acceleration(const State &state,
		const HydroDiagnostics &diagnostics) const;

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
	double top_pressure_pa() const { return top_pressure_pa_; }
	double gravity_mps2() const { return gravity_mps2_; }

private:
	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
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
