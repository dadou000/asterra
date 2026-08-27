#pragma once

#include "voronoi_dry_dynamics.h"
#include "voronoi_dry_vertical_transport.h"

#include <array>
#include <vector>

namespace asterra::weather {

// Transactional 30-level atmosphere integrator. Horizontal dynamics and
// pressure-coordinate remapping form one transaction; dry mass, theta mass,
// passive tracer masses and edge momentum roll back together if any positivity
// or conservation gate fails. Optional moist pressure feedback changes only the
// diagnosed pressure/geopotential force inside the horizontal SSPRK stages;
// the conservative vertical coordinate remains dry-mass based.
class VoronoiDryCore {
public:
	static constexpr int LEVELS = VoronoiDryDynamics::LEVELS;
	using State = VoronoiDryDynamics::State;
	using MoistTracerIndices = VoronoiDryDynamics::MoistTracerIndices;

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
		double max_pressure_acceleration_mps2 = 0.0;
		double max_coordinate_mass_fraction_error = 0.0;
		double max_coordinate_column_mass_error = 0.0;
		double max_coordinate_column_theta_mass_error = 0.0;
		double max_coordinate_column_tracer_mass_error = 0.0;
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

	void set_surface_height_m(const std::vector<double> &height_m) {
		dynamics_.set_surface_height_m(height_m);
		vertical_.set_surface_height_m(height_m);
	}
	void set_surface_geopotential_m2_s2(const std::vector<double> &geopotential) {
		dynamics_.set_surface_geopotential_m2_s2(geopotential);
		vertical_.set_surface_geopotential_m2_s2(geopotential);
	}
	void set_moist_pressure_feedback(bool enabled,
		MoistTracerIndices indices = {}) {
		dynamics_.set_moist_pressure_feedback(enabled, indices);
	}
	bool moist_pressure_feedback_enabled() const {
		return dynamics_.moist_pressure_feedback_enabled();
	}

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const {
		return dynamics_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	}
	State make_isothermal_terrain_balanced_reference(double reference_surface_pressure_pa,
		double temperature_k) const {
		return dynamics_.make_isothermal_terrain_balanced_reference(
			reference_surface_pressure_pa, temperature_k);
	}

	double total_dry_mass_kg(const State &state) const {
		return dynamics_.total_dry_mass_kg(state);
	}
	double total_theta_mass_kg_k(const State &state) const {
		return dynamics_.total_theta_mass_kg_k(state);
	}
	double total_tracer_mass_kg(const State &state, int tracer) const {
		return dynamics_.total_tracer_mass_kg(state, tracer);
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
