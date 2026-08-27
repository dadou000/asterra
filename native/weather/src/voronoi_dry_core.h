#pragma once

#include "voronoi_dry_dynamics.h"
#include "voronoi_dry_vertical_transport.h"

#include <array>
#include <vector>

namespace asterra::weather {

// Transactional 30-level dry-atmosphere integrator.
//
// VoronoiDryDynamics advances the horizontal primitive-equation state on a
// temporarily deforming mass coordinate. After every accepted horizontal
// update, VoronoiDryVerticalTransport conservatively remaps that state back to
// the reference pressure-coordinate layer fractions. The horizontal advance
// and coordinate remap are one transaction: any invalid/remap/conservation
// failure restores the complete pre-step state.
//
// This is the runtime-facing dry dynamical core. Explicit vertical physics,
// moisture and surface forcing remain separate future source/transport terms.
class VoronoiDryCore {
public:
	static constexpr int LEVELS = VoronoiDryDynamics::LEVELS;
	using State = VoronoiDryDynamics::State;

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
		double max_pressure_acceleration_mps2 = 0.0;
		double max_coordinate_mass_fraction_error = 0.0;
		double max_coordinate_column_mass_error = 0.0;
		double max_coordinate_column_theta_mass_error = 0.0;
		double max_coordinate_edge_momentum_error = 0.0;
		int rejected_steps = 0;
		bool coordinate_remap_applied = false;
	};

	VoronoiDryCore(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0,
		double top_pressure_pa = 7500.0,
		double rotation_rate_rad_s = 0.0,
		Vec3d rotation_axis = {0.0, 1.0, 0.0});

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const {
		return dynamics_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	}

	double total_dry_mass_kg(const State &state) const {
		return dynamics_.total_dry_mass_kg(state);
	}
	double total_theta_mass_kg_k(const State &state) const {
		return dynamics_.total_theta_mass_kg_k(state);
	}
	double total_dry_energy_j(const State &state) const {
		return dynamics_.total_dry_energy_j(state);
	}
	double total_relative_axial_angular_momentum_kg_m2_s(const State &state) const {
		return dynamics_.total_relative_axial_angular_momentum_kg_m2_s(state);
	}
	double total_absolute_axial_angular_momentum_kg_m2_s(const State &state) const {
		return dynamics_.total_absolute_axial_angular_momentum_kg_m2_s(state);
	}

	std::vector<Vec3d> reconstruct_cell_velocity(const State &state,
		int level) const {
		return dynamics_.reconstruct_cell_velocity(state, level);
	}
	std::vector<double> reconstruct_vertex_relative_vorticity(const State &state,
		int level) const {
		return dynamics_.reconstruct_vertex_relative_vorticity(state, level);
	}

	double max_coordinate_mass_fraction_error(const State &state) const;

	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.30, int max_retries = 10) const;

	const VoronoiDryDynamics &dynamics() const { return dynamics_; }
	const VoronoiDryTransport &transport() const { return dynamics_.transport(); }
	const VoronoiDryVerticalTransport &vertical_transport() const { return vertical_; }
	const GeodesicVoronoiGrid &grid() const { return *grid_; }
	const std::array<double, LEVELS> &reference_mass_fractions() const {
		return vertical_.reference_mass_fractions();
	}

private:
	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryDynamics dynamics_;
	VoronoiDryVerticalTransport vertical_;

	void refresh_extrema(const State &state, StepDiagnostics &diagnostics) const;
};

} // namespace asterra::weather
