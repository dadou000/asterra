#pragma once

#include "voronoi_dry_dynamics.h"
#include "voronoi_dry_vertical_transport.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace asterra::weather {

// Transactional 30-level atmosphere integrator. Horizontal dynamics and
// pressure-coordinate remapping form one transaction; dry mass, theta mass,
// passive tracer masses and edge momentum roll back together if any positivity
// or conservation gate fails. Optional moist pressure feedback changes only the
// diagnosed pressure/geopotential force inside the horizontal SSPRK stages;
// the conservative vertical coordinate remains dry-mass based.
//
// A scale-aware divergence damper is applied as a separate SSPRK3 momentum
// source after coordinate remapping. It targets fast compressive/grid-scale
// modes through +nu_D grad(div u), with nu_D = C_D * c * d_edge. Because it
// changes only edge-normal wind, conservative scalar/tracer budgets are not
// altered. The complete dry-core transaction rolls back if the filter becomes
// anti-diffusive or numerically invalid.
class VoronoiDryCore {
public:
	static constexpr int LEVELS = VoronoiDryDynamics::LEVELS;
	static constexpr double DEFAULT_DIVERGENCE_DAMPING_STRENGTH = 0.12;
	static constexpr double MAX_DIVERGENCE_DAMPING_STRENGTH = 0.35;
	using State = VoronoiDryDynamics::State;
	using MoistTracerIndices = VoronoiDryDynamics::MoistTracerIndices;

	struct DivergenceDampingDiagnostics {
		double requested_dt_s = 0.0;
		double applied_dt_s = 0.0;
		double strength = 0.0;
		double rms_divergence_before_s1 = 0.0;
		double rms_divergence_after_s1 = 0.0;
		double max_abs_divergence_before_s1 = 0.0;
		double max_abs_divergence_after_s1 = 0.0;
		double max_damping_acceleration_mps2 = 0.0;
		double max_diffusive_courant = 0.0;
		int substeps = 0;
		bool applied = false;
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
		double max_pressure_acceleration_mps2 = 0.0;
		double max_coordinate_mass_fraction_error = 0.0;
		double max_coordinate_column_mass_error = 0.0;
		double max_coordinate_column_theta_mass_error = 0.0;
		double max_coordinate_column_tracer_mass_error = 0.0;
		double max_coordinate_edge_momentum_error = 0.0;
		double divergence_rms_before_s1 = 0.0;
		double divergence_rms_after_s1 = 0.0;
		double max_abs_divergence_before_s1 = 0.0;
		double max_abs_divergence_after_s1 = 0.0;
		double max_divergence_damping_acceleration_mps2 = 0.0;
		double max_divergence_damping_courant = 0.0;
		int divergence_damping_substeps = 0;
		int rejected_steps = 0;
		bool coordinate_remap_applied = false;
		bool divergence_damping_applied = false;
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
		const std::array<int, 5> all{
			indices.vapor, indices.cloud_liquid, indices.cloud_ice,
			indices.rain, indices.snow};
		for (int index : all) {
			if (index < 0) {
				throw std::invalid_argument(
					"Moist pressure-feedback tracer indices must be non-negative");
			}
		}
		for (size_t a = 0; a < all.size(); ++a) {
			for (size_t b = a + 1; b < all.size(); ++b) {
				if (all[a] == all[b]) {
					throw std::invalid_argument(
						"Moist pressure-feedback water tracer indices must be distinct");
				}
			}
		}
		dynamics_.set_moist_pressure_feedback(enabled, indices);
	}
	bool moist_pressure_feedback_enabled() const {
		return dynamics_.moist_pressure_feedback_enabled();
	}

	// Dimensionless C_D in nu_D = C_D * c * d_edge. Zero disables damping.
	// Values above MAX_DIVERGENCE_DAMPING_STRENGTH are rejected; strong values
	// automatically subcycle the explicit SSPRK3 diffusion source.
	void set_divergence_damping_strength(double strength);
	double divergence_damping_strength() const {
		return divergence_damping_strength_;
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

	// Native C-grid horizontal divergence, one value per scalar Voronoi cell.
	std::vector<double> horizontal_divergence_s1(const State &state,
		int level) const;
	// Area-weighted RMS over every cell and vertical level.
	double rms_horizontal_divergence_s1(const State &state) const;
	// Public mainly for diagnostics/regression tests; normal runtime stepping
	// invokes the same source automatically after coordinate remapping.
	DivergenceDampingDiagnostics apply_divergence_damping(
		State &state, double dt_s) const;

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
	double divergence_damping_strength_ = DEFAULT_DIVERGENCE_DAMPING_STRENGTH;

	void refresh_extrema(const State &state, StepDiagnostics &diagnostics) const;
};

} // namespace asterra::weather
