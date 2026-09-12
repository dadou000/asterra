#pragma once

#include "geodesic_voronoi_grid.h"
#include "voronoi_dry_transport.h"
#include "voronoi_moist_hydrostatic.h"

#include <vector>

namespace asterra::weather {

// Coupled horizontal primitive-equation prototype on the spherical Voronoi
// C-grid. It advances conservative dry mass, mass-weighted potential
// temperature, any passive tracer masses, and edge-normal wind together.
// Tracers use the exact same donor dry-mass flux as thermodynamics.
//
// Moist pressure feedback is opt-in. When enabled, every SSPRK tendency stage
// diagnoses total mechanical pressure and virtual-temperature geopotential from
// the configured vapor/liquid/ice tracer slots. Dry mass remains the transport
// and vertical-coordinate mass, matching the existing conservative state.
class VoronoiDryDynamics {
public:
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;
	using State = VoronoiDryTransport::State;
	using MoistTracerIndices = VoronoiMoistThermodynamics::TracerIndices;

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
		int rejected_steps = 0;
	};

	VoronoiDryDynamics(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0,
		double top_pressure_pa = 7500.0,
		double rotation_rate_rad_s = 0.0,
		Vec3d rotation_axis = {0.0, 1.0, 0.0});

	void set_surface_height_m(const std::vector<double> &height_m) {
		transport_.set_surface_height_m(height_m);
	}
	void set_surface_geopotential_m2_s2(const std::vector<double> &geopotential) {
		transport_.set_surface_geopotential_m2_s2(geopotential);
	}

	// Pressure/geopotential feedback only. Scalar/tracer transport and the dry
	// pressure coordinate remain unchanged. Enabling this requires the configured
	// water tracer slots to exist in every state subsequently stepped.
	void set_moist_pressure_feedback(bool enabled,
		MoistTracerIndices indices = {});
	bool moist_pressure_feedback_enabled() const {
		return moist_pressure_feedback_enabled_;
	}
	const MoistTracerIndices &moist_tracer_indices() const { return moist_indices_; }

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const {
		return transport_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	}
	State make_isothermal_terrain_balanced_reference(double reference_surface_pressure_pa,
		double temperature_k) const {
		return transport_.make_isothermal_terrain_balanced_reference(
			reference_surface_pressure_pa, temperature_k);
	}

	double total_dry_mass_kg(const State &state) const {
		return transport_.total_dry_mass_kg(state);
	}
	double total_theta_mass_kg_k(const State &state) const {
		return transport_.total_theta_mass_kg_k(state);
	}
	double total_tracer_mass_kg(const State &state, int tracer) const {
		return transport_.total_tracer_mass_kg(state, tracer);
	}

	std::vector<Vec3d> reconstruct_cell_velocity(const State &state,
		int level) const;
	std::vector<double> reconstruct_vertex_relative_vorticity(const State &state,
		int level) const;

	double total_dry_energy_j(const State &state) const;
	double total_relative_axial_angular_momentum_kg_m2_s(const State &state) const;
	double total_absolute_axial_angular_momentum_kg_m2_s(const State &state) const;

	double max_courant(const State &state, double dt_s) const;
	double stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const;

	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.30, int max_retries = 10) const;

	const VoronoiDryTransport &transport() const { return transport_; }
	const GeodesicVoronoiGrid &grid() const { return *grid_; }
	double rotation_rate_rad_s() const { return rotation_rate_rad_s_; }
	const Vec3d &rotation_axis() const { return rotation_axis_; }

private:
	struct CellReconstruction {
		std::vector<Vec3d> coefficient;
	};

	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
		std::vector<std::vector<double>> tracer_mass_dt;
		std::vector<double> edge_velocity_dt;
		std::vector<double> normal_mass_flux; // dry m*u, kg/(m s), globally a -> b
		double max_pressure_acceleration_mps2 = 0.0;
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryTransport transport_;
	double rotation_rate_rad_s_ = 0.0;
	Vec3d rotation_axis_{0.0, 1.0, 0.0};
	std::vector<CellReconstruction> reconstruction_;
	bool moist_pressure_feedback_enabled_ = false;
	MoistTracerIndices moist_indices_{};

	int scalar_index(int level, int cell) const {
		return level * grid_->cell_count() + cell;
	}
	int edge_index(int level, int edge) const {
		return level * grid_->edge_count() + edge;
	}

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	void build_reconstruction();
	Vec3d reconstruct_cell_vector_from_edges(const State &state,
		int cell, int level) const;
	std::vector<double> cell_kinetic_energy(const State &state, int level) const;
	Tendencies compute_tendencies(const State &state) const;
	bool euler_stage(const State &input, State &output, double dt_s,
		double &max_pressure_acceleration_mps2) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s,
		double &max_pressure_acceleration_mps2) const;
};

} // namespace asterra::weather
