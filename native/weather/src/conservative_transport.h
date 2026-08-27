#pragma once

#include "cubed_sphere_grid.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

namespace asterra::weather {

class ConservativeTransport2D {
public:
	struct SharedEdge {
		int cell_a = -1;
		int edge_a = -1;
		int cell_b = -1;
		int edge_b = -1;
		double length_m = 0.0;
		Vec3d midpoint;
		// Tangent-plane unit normal pointing from cell_a toward cell_b.
		Vec3d normal_a_to_b;
	};

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double used_dt_s = 0.0;
		double max_courant = 0.0;
		double mass_before = 0.0;
		double mass_after = 0.0;
		double relative_mass_error = 0.0;
		double min_density = 0.0;
		double max_density = 0.0;
		std::size_t positivity_limiter_activations = 0;
	};

	using VelocityFunction = std::function<Vec3d(const Vec3d &unit_position)>;

	explicit ConservativeTransport2D(const CubedSphereGrid &grid);

	const CubedSphereGrid &grid() const { return *grid_; }
	const std::vector<SharedEdge> &shared_edges() const { return edges_; }

	// Sample a physical tangent velocity field at each shared edge midpoint and
	// return its normal component in m/s. One value exists per physical edge.
	std::vector<double> sample_edge_normal_velocity(const VelocityFunction &velocity) const;

	// CFL is based on the total outward swept area rate from each finite-volume
	// cell: dt * sum(max(u_n,0) * edge_length) / cell_area.
	double stable_dt(const std::vector<double> &edge_normal_velocity_mps,
		double target_cfl = 0.45,
		double maximum_dt_s = std::numeric_limits<double>::infinity()) const;
	double max_courant(const std::vector<double> &edge_normal_velocity_mps,
		double dt_s) const;

	// Advance a non-negative areal density (kg/m^2, tracer mass/m^2, etc.) using
	// flux-form finite volume, MC-limited MUSCL reconstruction and SSPRK3.
	// The timestep is automatically reduced to satisfy target_cfl. Shared edge
	// fluxes are applied once with equal/opposite signs, so total mass closes to
	// round-off. A donor outflow limiter guarantees positivity without clipping
	// the final state.
	StepDiagnostics step_ssprk3(std::vector<double> &density,
		const std::vector<double> &edge_normal_velocity_mps,
		double requested_dt_s,
		double target_cfl = 0.45) const;

	double total_mass(const std::vector<double> &density) const;

private:
	const CubedSphereGrid *grid_ = nullptr;
	std::vector<SharedEdge> edges_;

	static int opposite_edge(int edge);
	static double great_circle_distance_m(const Vec3d &a, const Vec3d &b, double radius_m);
	static double minmod3(double a, double b, double c);

	double reconstruct_outflow_density(const std::vector<double> &density,
		int donor_cell, int donor_edge, int receiver_cell,
		const Vec3d &edge_midpoint) const;

	std::size_t euler_stage(const std::vector<double> &input,
		std::vector<double> &output,
		const std::vector<double> &edge_normal_velocity_mps,
		double dt_s) const;
};

} // namespace asterra::weather
