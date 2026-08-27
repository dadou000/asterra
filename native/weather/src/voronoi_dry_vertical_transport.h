#pragma once

#include "voronoi_dry_transport.h"

#include <vector>

namespace asterra::weather {

// Conservative vertical mass exchange for the 30-level dry atmosphere.
//
// Interface j lies between level j (below) and j+1 (above). Positive interface
// mass flux is upward from j -> j+1; negative is downward. Flux units are
// kg/m2/s. The same donor mass transfer carries mass-weighted potential
// temperature, so every closed column conserves dry mass and theta mass exactly
// apart from floating-point summation. Edge winds are untouched.
//
// This operator intentionally accepts the interface mass flux as input. The
// subsequent dynamics slice will diagnose that flux from horizontal mass
// convergence / coordinate remap; keeping transport separate makes its budget
// and positivity properties independently testable.
class VoronoiDryVerticalTransport {
public:
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;
	static constexpr int INTERFACES = LEVELS - 1;
	using State = VoronoiDryTransport::State;

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_vertical_courant = 0.0;
		double dry_mass_before_kg = 0.0;
		double dry_mass_after_kg = 0.0;
		double relative_dry_mass_error = 0.0;
		double theta_mass_before_kg_k = 0.0;
		double theta_mass_after_kg_k = 0.0;
		double relative_theta_mass_error = 0.0;
		double min_layer_mass_kg_m2 = 0.0;
		double min_potential_temperature_k = 0.0;
		int rejected_steps = 0;
	};

	explicit VoronoiDryVerticalTransport(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0,
		double top_pressure_pa = 7500.0);

	// Convenience reference state; physics/state representation is shared with
	// VoronoiDryTransport and VoronoiDryDynamics.
	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const {
		return horizontal_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	}

	// interface_mass_flux is [interface][cell], size (LEVELS-1)*cell_count.
	double max_courant(const State &state,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
	double stable_dt(const State &state,
		const std::vector<double> &interface_mass_flux,
		double target_cfl, double maximum_dt_s) const;

	StepDiagnostics step(State &state,
		const std::vector<double> &interface_mass_flux,
		double requested_dt_s, double target_cfl = 0.45,
		int max_retries = 10) const;

	const VoronoiDryTransport &horizontal_transport() const { return horizontal_; }

private:
	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryTransport horizontal_;

	int scalar_index(int level, int cell) const {
		return level * grid_->cell_count() + cell;
	}
	int interface_index(int interface_level, int cell) const {
		return interface_level * grid_->cell_count() + cell;
	}

	void validate_state(const State &state) const;
	void validate_flux(const std::vector<double> &interface_mass_flux) const;
	bool finite_positive(const State &state) const;
	Tendencies compute_tendencies(const State &state,
		const std::vector<double> &interface_mass_flux) const;
	bool euler_stage(const State &input, State &output,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
};

} // namespace asterra::weather
