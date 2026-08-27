#include "voronoi_shallow_water.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiShallowWater;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::normalized;

static constexpr double PI = 3.141592653589793238462643383279502884;
static constexpr double R = 3500000.0;
static constexpr double G = 9.80665;
static constexpr double ROTATION_PERIOD = 11.5 * 3600.0;
static constexpr double OMEGA = 2.0 * PI / ROTATION_PERIOD;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

struct Tc2Result {
	double depth_error = 0.0;
	double velocity_error = 0.0;
	double mass_error = 0.0;
	double energy_error = 0.0;
	double min_depth = 0.0;
	double max_depth = 0.0;
	double max_speed = 0.0;
	int steps = 0;
};

static Tc2Result run_tc2(int frequency, double rotations) {
	GeodesicVoronoiGrid grid(frequency, R);
	const Vec3d axis = normalized(Vec3d{0.31, 0.89, -0.335});
	VoronoiShallowWater sw(grid, G, OMEGA, axis);
	constexpr double H0 = 5000.0;
	const double flow_rate = OMEGA / 12.0;
	const double u0 = flow_rate * R;
	const double amplitude = (OMEGA * flow_rate + 0.5 * flow_rate * flow_rate) * R * R / G;
	require(H0 - amplitude > 1000.0, "TC2 base layer is too shallow");

	auto state = sw.make_uniform_state(H0);
	for (int c = 0; c < grid.cell_count(); ++c) {
		const double mu = dot(axis, grid.cell(c).center);
		state.depth_m[static_cast<size_t>(c)] = H0 - amplitude * mu * mu;
	}
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		const Vec3d analytic = cross(axis, edge.midpoint) * u0;
		state.edge_normal_mps[static_cast<size_t>(e)] = dot(analytic, edge.normal_a_to_b);
	}
	const auto initial = state;
	const double mass0 = sw.total_volume_m3(state);
	const double energy0 = sw.total_energy(state);

	const double duration = rotations * ROTATION_PERIOD;
	double elapsed = 0.0;
	int steps = 0;
	while (elapsed < duration) {
		const double request = std::min(600.0, duration - elapsed);
		const auto diag = sw.step(state, request, 0.28);
		require(diag.accepted_dt_s > 0.0, "TC2 timestep collapsed");
		require(diag.max_courant <= 0.2940000001, "TC2 accepted CFL exceeded safety envelope");
		require(diag.min_depth_m > 0.0, "TC2 produced non-positive depth");
		elapsed += diag.accepted_dt_s;
		++steps;
		require(steps < 30000, "TC2 used excessive timesteps");
	}

	long double h_error2 = 0.0L;
	long double area_sum = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const long double area = grid.cell(c).area_m2;
		const long double dh = state.depth_m[static_cast<size_t>(c)] - initial.depth_m[static_cast<size_t>(c)];
		h_error2 += dh * dh * area;
		area_sum += area;
	}
	long double u_error2 = 0.0L;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const long double du = state.edge_normal_mps[static_cast<size_t>(e)]
			- initial.edge_normal_mps[static_cast<size_t>(e)];
		u_error2 += du * du;
	}

	Tc2Result result;
	result.depth_error = std::sqrt(static_cast<double>(h_error2 / area_sum)) / amplitude;
	result.velocity_error = std::sqrt(static_cast<double>(u_error2
		/ static_cast<long double>(grid.edge_count()))) / u0;
	result.mass_error = std::abs(sw.total_volume_m3(state) - mass0) / mass0;
	result.energy_error = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	result.min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	result.max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	for (double u : state.edge_normal_mps) result.max_speed = std::max(result.max_speed, std::abs(u));
	result.steps = steps;
	return result;
}

int main() {
	try {
		constexpr int F = 10;
		GeodesicVoronoiGrid grid(F, R);
		const Vec3d axis = normalized(Vec3d{0.37, 0.81, -0.455});
		VoronoiShallowWater sw(grid, G, OMEGA, axis);

		// Exact rest must remain bitwise unchanged; this is a fundamental no-source
		// state for the eventual hydrostatic atmosphere.
		auto rest = sw.make_uniform_state(4200.0);
		const auto rest_before = rest;
		const auto rest_diag = sw.step(rest, 7200.0, 0.30);
		require(rest.depth_m == rest_before.depth_m, "Voronoi lake-at-rest depth changed");
		require(rest.edge_normal_mps == rest_before.edge_normal_mps, "Voronoi lake-at-rest velocity changed");
		require(rest_diag.relative_mass_error == 0.0, "Voronoi lake-at-rest mass changed");

		// Smooth non-trivial state used to test the spatial Hamiltonian identity.
		auto energetic = sw.make_uniform_state(5000.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const Vec3d p = grid.cell(c).center;
			energetic.depth_m[static_cast<size_t>(c)] += 180.0 * p.x * p.z + 70.0 * p.y;
		}
		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			const Vec3d velocity = cross(axis, edge.midpoint) * 38.0
				+ cross(normalized(Vec3d{-0.4, 0.2, 0.89}), edge.midpoint) * 11.0;
			energetic.edge_normal_mps[static_cast<size_t>(e)] = dot(velocity, edge.normal_a_to_b);
		}
		const double energy = sw.total_energy(energetic);
		const double energy_tendency = sw.instantaneous_energy_tendency(energetic);
		const double relative_energy_tendency_per_s = std::abs(energy_tendency) / std::max(std::abs(energy), 1.0);
		require(relative_energy_tendency_per_s < 2e-12,
			"Voronoi semi-discrete operator injects/removes energy");

		// The circulation of relative vorticity over the closed dual mesh must
		// cancel because every Delaunay edge is traversed once in each direction.
		const auto zeta = sw.reconstruct_vertex_relative_vorticity(energetic);
		long double global_circulation = 0.0L;
		long double absolute_circulation = 0.0L;
		for (int v = 0; v < grid.vertex_count(); ++v) {
			const long double contribution = static_cast<long double>(zeta[static_cast<size_t>(v)])
				* static_cast<long double>(grid.vertex(v).dual_area_m2);
			global_circulation += contribution;
			absolute_circulation += std::abs(contribution);
		}
		const double circulation_residual = static_cast<double>(
			std::abs(global_circulation) / std::max(absolute_circulation, 1.0L));
		require(circulation_residual < 2e-13, "dual-mesh relative vorticity does not close globally");

		// Direct cell-to-cell Bernoulli differences must respond to a high-frequency
		// thickness perturbation; no collocated pressure checkerboard null space.
		auto pressure_mode = sw.make_uniform_state(5000.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const Vec3d p = grid.cell(c).center;
			pressure_mode.depth_m[static_cast<size_t>(c)] += ((c * 37) & 1) ? 45.0 : -45.0;
			pressure_mode.depth_m[static_cast<size_t>(c)] += 8.0 * p.x;
		}
		const auto pressure_before = pressure_mode;
		const auto pressure_diag = sw.step(pressure_mode, 30.0, 0.20);
		int accelerated = 0;
		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			if (std::abs(pressure_before.depth_m[static_cast<size_t>(edge.cell_a)]
					- pressure_before.depth_m[static_cast<size_t>(edge.cell_b)]) > 1.0
					&& std::abs(pressure_mode.edge_normal_mps[static_cast<size_t>(e)]) > 1e-8) {
				++accelerated;
			}
		}
		require(accelerated > grid.edge_count() / 3, "Voronoi pressure operator has an excessive null space");
		require(pressure_diag.relative_mass_error < 2e-13, "pressure-mode step lost mass");

		// Run beyond the failure window of the rejected cubed-sphere nonlinear core.
		const Tc2Result tc2_coarse = run_tc2(8, 5.0);
		const Tc2Result tc2_fine = run_tc2(12, 5.0);
		require(tc2_coarse.mass_error < 2e-11 && tc2_fine.mass_error < 2e-11,
			"Voronoi TC2 drifted in total mass");
		require(tc2_coarse.energy_error < 2e-3 && tc2_fine.energy_error < 2e-3,
			"Voronoi TC2 drifted excessively in total energy");
		require(tc2_coarse.min_depth > 1000.0 && tc2_fine.min_depth > 1000.0,
			"Voronoi TC2 developed an unphysical thin layer");
		require(tc2_coarse.max_speed < 180.0 && tc2_fine.max_speed < 180.0,
			"Voronoi TC2 wind accelerated unrealistically");
		require(tc2_fine.depth_error < tc2_coarse.depth_error * 1.15,
			"Voronoi TC2 height error worsened materially with refinement");
		require(tc2_fine.velocity_error < tc2_coarse.velocity_error * 1.15,
			"Voronoi TC2 velocity error worsened materially with refinement");

		std::cout << "VoronoiShallowWater PASS\n"
			<< "  spatial relative dE/dt per s: " << relative_energy_tendency_per_s << "\n"
			<< "  global circulation residual: " << circulation_residual << "\n"
			<< "  pressure-mode accelerated edges: " << accelerated << "/" << grid.edge_count() << "\n"
			<< "  TC2 coarse steps/error h/u/mass/energy: " << tc2_coarse.steps << " "
			<< tc2_coarse.depth_error << " " << tc2_coarse.velocity_error << " "
			<< tc2_coarse.mass_error << " " << tc2_coarse.energy_error << "\n"
			<< "  TC2 fine steps/error h/u/mass/energy: " << tc2_fine.steps << " "
			<< tc2_fine.depth_error << " " << tc2_fine.velocity_error << " "
			<< tc2_fine.mass_error << " " << tc2_fine.energy_error << "\n"
			<< "  TC2 fine min/max depth, max speed: " << tc2_fine.min_depth << " "
			<< tc2_fine.max_depth << " " << tc2_fine.max_speed << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiShallowWater FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
