#include "shallow_water_cgrid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::CubedSphereGrid;
using asterra::weather::ShallowWaterCGrid;
using asterra::weather::Vec3d;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::length;
using asterra::weather::normalized;

static constexpr double PI = 3.141592653589793238462643383279502884;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

static double angular_distance(const Vec3d &a, const Vec3d &b) {
	return std::atan2(length(cross(a, b)), std::clamp(dot(a, b), -1.0, 1.0));
}

int main() {
	try {
		constexpr int N = 24;
		constexpr double R = 3500000.0;
		constexpr double G = 9.80665;
		constexpr double H = 10000.0;
		CubedSphereGrid grid(N, R);
		ShallowWaterCGrid sw(grid, G);

		// Gate 1: exact lake-at-rest state. Even an absurdly large requested dt is
		// reduced to the gravity-wave CFL envelope, but the state itself must remain
		// bitwise untouched because its RHS is identically zero.
		auto rest = sw.make_uniform_state(H);
		const auto rest_before = rest;
		const auto rest_diag = sw.step(rest, 86400.0, 0.40);
		require(rest.depth_m == rest_before.depth_m, "lake-at-rest depth changed");
		require(rest.edge_normal_mps == rest_before.edge_normal_mps, "lake-at-rest velocity changed");
		require(rest_diag.accepted_dt_s < rest_diag.requested_dt_s,
			"gravity-wave CFL did not constrain a huge rest-state request");
		require(rest_diag.max_wave_courant <= 0.400000000001,
			"rest-state accepted CFL exceeded target");
		require(rest_diag.relative_mass_error == 0.0, "lake-at-rest mass changed");

		// Gate 2: explicitly challenge the old A-grid null mode. A cell-centered
		// +/- checkerboard on one cube face must generate edge acceleration because
		// each C-grid velocity lies directly between the two scalar cells.
		auto checker = sw.make_uniform_state(H);
		constexpr double A = 80.0;
		for (int j = 2; j < N - 2; ++j) {
			for (int i = 2; i < N - 2; ++i) {
				const int c = grid.cell_id(CubedSphereGrid::POS_X, i, j);
				checker.depth_m[c] += ((i + j) & 1) ? A : -A;
			}
		}
		const double checker_volume0 = sw.total_volume_m3(checker);
		const auto checker_diag = sw.step(checker, 120.0, 0.30);
		require(checker_diag.max_wave_courant <= 0.315000000001,
			"checkerboard step exceeded accepted CFL tolerance");
		const double checker_volume_error = std::abs(sw.total_volume_m3(checker) - checker_volume0)
			/ checker_volume0;
		require(checker_volume_error < 2e-13, "checkerboard test lost layer mass");

		int active_interior_edges = 0;
		int tested_interior_edges = 0;
		for (size_t e = 0; e < sw.topology().shared_edges().size(); ++e) {
			const auto &edge = sw.topology().shared_edges()[e];
			const auto a = grid.address(edge.cell_a);
			const auto b = grid.address(edge.cell_b);
			if (a.face != CubedSphereGrid::POS_X || b.face != CubedSphereGrid::POS_X) continue;
			if (a.i < 3 || a.i >= N - 3 || a.j < 3 || a.j >= N - 3) continue;
			if (b.i < 3 || b.i >= N - 3 || b.j < 3 || b.j >= N - 3) continue;
			++tested_interior_edges;
			if (std::abs(checker.edge_normal_mps[e]) > 1e-5) ++active_interior_edges;
		}
		require(tested_interior_edges > 100, "checkerboard test sampled too few interior edges");
		require(active_interior_edges > tested_interior_edges * 9 / 10,
			"C-grid pressure operator contains a checkerboard-like null mode");

		// Gate 3: compact free-surface perturbation launches gravity waves for half
		// a simulated day. It may disperse/damp, but must remain positive, finite,
		// mass-conservative and bounded without any polar or pressure smoothing.
		auto wave = sw.make_uniform_state(H);
		const Vec3d pulse_center = normalized(Vec3d{0.41, -0.72, 0.56});
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double angle = angular_distance(grid.cell(c).center, pulse_center);
			wave.depth_m[c] += 65.0 * std::exp(-(angle * angle) / (2.0 * 0.12 * 0.12));
		}
		const double wave_volume0 = sw.total_volume_m3(wave);
		const double initial_max_anomaly = *std::max_element(wave.depth_m.begin(), wave.depth_m.end()) - H;
		double elapsed = 0.0;
		double worst_cfl = 0.0;
		double worst_mass_error = 0.0;
		double max_speed = 0.0;
		int total_rejections = 0;
		int steps = 0;
		constexpr double DURATION = 0.5 * 86400.0;
		while (elapsed < DURATION) {
			const double request = std::min(900.0, DURATION - elapsed);
			const auto diag = sw.step(wave, request, 0.38);
			require(diag.accepted_dt_s > 0.0, "gravity-wave timestep collapsed");
			require(diag.max_wave_courant <= 0.399000000001,
				"gravity-wave accepted step exceeded safety envelope");
			require(diag.min_depth_m > 0.0, "gravity-wave run produced non-positive depth");
			worst_cfl = std::max(worst_cfl, diag.max_wave_courant);
			worst_mass_error = std::max(worst_mass_error, diag.relative_mass_error);
			max_speed = std::max(max_speed, diag.max_speed_mps);
			total_rejections += diag.rejected_steps;
			elapsed += diag.accepted_dt_s;
			++steps;
			require(steps < 5000, "gravity-wave test used excessive timesteps");
		}
		const double wave_volume_error = std::abs(sw.total_volume_m3(wave) - wave_volume0)
			/ wave_volume0;
		require(wave_volume_error < 2e-11, "gravity-wave run drifted in total layer mass");
		const double max_depth = *std::max_element(wave.depth_m.begin(), wave.depth_m.end());
		const double min_depth = *std::min_element(wave.depth_m.begin(), wave.depth_m.end());
		require(max_depth - H < initial_max_anomaly * 2.0,
			"gravity-wave free-surface amplitude grew without bound");
		require(H - min_depth < initial_max_anomaly * 2.0,
			"gravity-wave trough amplitude grew without bound");
		require(max_speed < 80.0, "gravity-wave velocity grew unrealistically large");

		// Gate 4: smooth spherical geostrophic/solid-body balance using Asterra's
		// actual 11.5 h rotation rate. The analytic state is a linear shallow-water
		// balance: U = W R (axis x r), h = H + Omega*W*R^2/g * cos^2(latitude).
		// There is no latitude-coordinate code in the solver; this test crosses all
		// cube seams and both geographic poles through pure vector geometry.
		constexpr double OMEGA = 2.0 * PI / (11.5 * 3600.0);
		constexpr double FLOW_ANGULAR_RATE = 1.0e-6;
		const Vec3d spin_axis{0.0, 1.0, 0.0};
		ShallowWaterCGrid rotating(grid, G, OMEGA, spin_axis);
		auto balanced = rotating.make_uniform_state(H);
		const double balance_amplitude = OMEGA * FLOW_ANGULAR_RATE * R * R / G;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double mu = dot(spin_axis, grid.cell(c).center);
			balanced.depth_m[c] = H + balance_amplitude * (1.0 - mu * mu);
		}
		balanced.edge_normal_mps = rotating.topology().sample_edge_normal_velocity(
			[spin_axis](const Vec3d &p) -> Vec3d {
				return cross(spin_axis, p) * (FLOW_ANGULAR_RATE * R);
			});
		const auto balanced_initial = balanced;
		const double balanced_volume0 = rotating.total_volume_m3(balanced);
		const double initial_max_speed = FLOW_ANGULAR_RATE * R;
		double balanced_elapsed = 0.0;
		int balanced_steps = 0;
		double balanced_worst_cfl = 0.0;
		constexpr double BALANCED_DURATION = 11.5 * 3600.0;
		while (balanced_elapsed < BALANCED_DURATION) {
			const double request = std::min(600.0, BALANCED_DURATION - balanced_elapsed);
			const auto diag = rotating.step(balanced, request, 0.34);
			require(diag.accepted_dt_s > 0.0, "balanced-flow timestep collapsed");
			balanced_worst_cfl = std::max(balanced_worst_cfl, diag.max_wave_courant);
			balanced_elapsed += diag.accepted_dt_s;
			++balanced_steps;
			require(balanced_steps < 5000, "balanced-flow test used excessive timesteps");
		}
		const double balanced_volume_error = std::abs(rotating.total_volume_m3(balanced) - balanced_volume0)
			/ balanced_volume0;
		require(balanced_volume_error < 2e-11, "balanced rotating flow drifted in total mass");

		long double depth_error_sq = 0.0L;
		long double velocity_error_sq = 0.0L;
		long double area_sum = 0.0L;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const long double area = static_cast<long double>(grid.cell(c).area_m2);
			const long double dh = static_cast<long double>(balanced.depth_m[c] - balanced_initial.depth_m[c]);
			depth_error_sq += dh * dh * area;
			area_sum += area;
		}
		for (size_t e = 0; e < balanced.edge_normal_mps.size(); ++e) {
			const long double du = static_cast<long double>(balanced.edge_normal_mps[e]
				- balanced_initial.edge_normal_mps[e]);
			velocity_error_sq += du * du;
		}
		const double depth_rms_error = std::sqrt(static_cast<double>(depth_error_sq / area_sum));
		const double velocity_rms_error = std::sqrt(static_cast<double>(
			velocity_error_sq / static_cast<long double>(balanced.edge_normal_mps.size())));
		require(depth_rms_error < balance_amplitude * 0.35,
			"balanced spherical flow developed excessive free-surface error");
		require(velocity_rms_error < initial_max_speed * 0.35,
			"balanced spherical flow developed excessive velocity error");
		const double balanced_max_speed = *std::max_element(
			balanced.edge_normal_mps.begin(), balanced.edge_normal_mps.end(),
			[](double a, double b) { return std::abs(a) < std::abs(b); });
		require(std::abs(balanced_max_speed) < initial_max_speed * 1.8,
			"balanced spherical flow accelerated without bound");

		std::cout << "ShallowWaterCGrid PASS\n"
			<< "  cells: " << grid.cell_count() << "\n"
			<< "  C-grid shared velocities: " << sw.topology().shared_edges().size() << "\n"
			<< "  checker active/tested interior edges: " << active_interior_edges << "/" << tested_interior_edges << "\n"
			<< "  checker mass error: " << checker_volume_error << "\n"
			<< "  gravity-wave steps: " << steps << "\n"
			<< "  worst wave CFL: " << worst_cfl << "\n"
			<< "  worst per-step mass error: " << worst_mass_error << "\n"
			<< "  total wave mass error: " << wave_volume_error << "\n"
			<< "  max wave speed m/s: " << max_speed << "\n"
			<< "  rejected/retried steps: " << total_rejections << "\n"
			<< "  rotating balance amplitude m: " << balance_amplitude << "\n"
			<< "  rotating balance steps: " << balanced_steps << "\n"
			<< "  rotating worst CFL: " << balanced_worst_cfl << "\n"
			<< "  rotating mass error: " << balanced_volume_error << "\n"
			<< "  rotating depth RMS error m: " << depth_rms_error << "\n"
			<< "  rotating velocity RMS error m/s: " << velocity_rms_error << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "ShallowWaterCGrid FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
