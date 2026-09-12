#pragma once

#include "geodesic_voronoi_grid.h"

#include <cstddef>
#include <vector>

namespace asterra::weather {

// Energy-compatible nonlinear shallow-water core on the spherical
// Voronoi/Delaunay C-grid. Layer thickness is cell centred, normal velocity is
// stored once per physical Voronoi edge, and relative vorticity/PV is stored on
// the triangular dual vertices. There is no latitude coordinate or polar branch.
class VoronoiShallowWater {
public:
	struct State {
		std::vector<double> depth_m;
		std::vector<double> edge_normal_mps;
	};

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_courant = 0.0;
		double mass_before_m3 = 0.0;
		double mass_after_m3 = 0.0;
		double relative_mass_error = 0.0;
		double energy_before_j_per_density = 0.0;
		double energy_after_j_per_density = 0.0;
		double relative_energy_change = 0.0;
		double min_depth_m = 0.0;
		double max_depth_m = 0.0;
		double max_speed_mps = 0.0;
		int rejected_steps = 0;
	};

	VoronoiShallowWater(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double rotation_rate_rad_s = 0.0,
		Vec3d rotation_axis = {0.0, 1.0, 0.0});

	State make_uniform_state(double depth_m) const;

	double total_volume_m3(const State &state) const;
	double total_energy(const State &state) const;
	double max_courant(const State &state, double dt_s) const;
	double stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const;

	std::vector<Vec3d> reconstruct_cell_velocity(const State &state) const;
	std::vector<double> reconstruct_vertex_relative_vorticity(const State &state) const;
	std::vector<double> reconstruct_vertex_potential_vorticity(const State &state) const;

	// Spatial semi-discrete energy identity. For a valid inviscid state this should
	// be roundoff-scale relative to the individual pressure/rotational work terms.
	double instantaneous_energy_tendency(const State &state) const;

	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.32, int max_retries = 10) const;

	const GeodesicVoronoiGrid &grid() const { return *grid_; }

private:
	struct CellReconstruction {
		// One coefficient vector per cell.edges[] entry. Multiplying each by the
		// corresponding globally-oriented edge scalar and summing reconstructs the
		// tangent vector at the cell centre.
		std::vector<Vec3d> coefficient;
	};

	struct Tendencies {
		std::vector<double> depth_dt;
		std::vector<double> edge_velocity_dt;
		std::vector<double> mass_flux;   // h_e u_e, globally oriented a -> b
		std::vector<double> bernoulli;   // g h + K at cells
		std::vector<double> q_edge;      // symmetric mean of endpoint PV
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	double gravity_mps2_ = 9.80665;
	double rotation_rate_rad_s_ = 0.0;
	Vec3d rotation_axis_{0.0, 1.0, 0.0};
	std::vector<CellReconstruction> reconstruction_;

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	void build_reconstruction();
	Vec3d reconstruct_cell_vector_from_edges(int cell,
		const std::vector<double> &edge_scalar) const;
	std::vector<double> cell_kinetic_energy(const State &state) const;
	Tendencies compute_tendencies(const State &state) const;
	bool euler_stage(const State &input, State &output, double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s) const;
};

} // namespace asterra::weather
