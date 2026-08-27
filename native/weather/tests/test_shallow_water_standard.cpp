#include "shallow_water_cgrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using asterra::weather::CubedSphereGrid;
using asterra::weather::ShallowWaterCGrid;
using asterra::weather::Vec3d;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::length;
using asterra::weather::normalized;

static constexpr double PI = 3.141592653589793238462643383279502884;
static constexpr double RADIUS_M = 3500000.0;
static constexpr double GRAVITY = 9.80665;
static constexpr double ROTATION_PERIOD_S = 11.5 * 3600.0;
static constexpr double OMEGA = 2.0 * PI / ROTATION_PERIOD_S;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

struct ErrorPair {
	double height = 0.0;
	double velocity = 0.0;
	double mass = 0.0;
	double energy = 0.0;
};

static ErrorPair run_williamson_tc2(int n) {
	CubedSphereGrid grid(n, RADIUS_M);
	// Tilt the physical rotation axis relative to all six cube faces. Since the
	// core has no geographic coordinate singularity, this is the clean cubed-
	// sphere analogue of Williamson TC2's rotated-coordinate stress test.
	const Vec3d axis = normalized(Vec3d{0.31, 0.89, -0.335});
	ShallowWaterCGrid sw(grid, GRAVITY, OMEGA, axis);

	constexpr double H0 = 3000.0;
	const double flow_angular_rate = OMEGA / 12.0;
	const double u0 = flow_angular_rate * RADIUS_M;
	const double amplitude = (OMEGA * flow_angular_rate
		+ 0.5 * flow_angular_rate * flow_angular_rate)
		* RADIUS_M * RADIUS_M / GRAVITY;
	require(H0 - amplitude > 200.0, "TC2 parameters make the layer non-positive");

	auto state = sw.make_uniform_state(H0);
	for (int c = 0; c < grid.cell_count(); ++c) {
		const double mu = dot(axis, grid.cell(c).center);
		state.depth_m[c] = H0 - amplitude * mu * mu;
	}
	state.edge_normal_mps = sw.topology().sample_edge_normal_velocity(
		[axis, u0](const Vec3d &p) -> Vec3d {
			return cross(axis, p) * u0;
		});
	const auto initial = state;
	const double mass0 = sw.total_volume_m3(state);
	const double energy0 = sw.total_energy(state);

	// Williamson TC2 is normally assessed after five planetary days. Scale that
	// nondimensional interval to Asterra's 11.5 h rotation period.
	const double duration = 5.0 * ROTATION_PERIOD_S;
	double elapsed = 0.0;
	int steps = 0;
	while (elapsed < duration) {
		const double request = std::min(600.0, duration - elapsed);
		const auto diag = sw.step(state, request, 0.34);
		require(diag.accepted_dt_s > 0.0, "TC2 timestep collapsed");
		require(diag.max_wave_courant <= 0.3570000001, "TC2 CFL gate failed");
		elapsed += diag.accepted_dt_s;
		++steps;
		require(steps < 20000, "TC2 used excessive timesteps");
	}

	long double h_error2 = 0.0L;
	long double area_sum = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const long double area = grid.cell(c).area_m2;
		const long double dh = state.depth_m[c] - initial.depth_m[c];
		h_error2 += dh * dh * area;
		area_sum += area;
	}
	long double u_error2 = 0.0L;
	for (size_t e = 0; e < state.edge_normal_mps.size(); ++e) {
		const long double du = state.edge_normal_mps[e] - initial.edge_normal_mps[e];
		u_error2 += du * du;
	}

	ErrorPair out;
	out.height = std::sqrt(static_cast<double>(h_error2 / area_sum)) / amplitude;
	out.velocity = std::sqrt(static_cast<double>(u_error2
		/ static_cast<long double>(state.edge_normal_mps.size()))) / u0;
	out.mass = std::abs(sw.total_volume_m3(state) - mass0) / mass0;
	out.energy = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	return out;
}

struct RossbyReference {
	double depth = 0.0;
	Vec3d velocity;
};

static RossbyReference rossby_haurwitz_reference(const Vec3d &p, double time_s) {
	constexpr int M = 4;
	// Preserve the standard Williamson nondimensional ratio omega/Omega while
	// scaling the benchmark to Asterra's rotation rate.
	const double ratio = 7.848e-6 / 7.292e-5;
	const double omega = OMEGA * ratio;
	const double K = omega;
	constexpr double H0 = 8000.0;

	const double sin_lat = std::clamp(p.y, -1.0, 1.0);
	const double lat = std::asin(sin_lat);
	const double cos_lat = std::max(std::cos(lat), 0.0);
	const double lon = std::atan2(-p.z, p.x);
	const double phase_speed =
		(static_cast<double>(M * (M + 3)) * omega - 2.0 * OMEGA)
		/ static_cast<double>((M + 1) * (M + 2));
	const double phase_lon = lon - phase_speed * time_s;

	const double c2 = cos_lat * cos_lat;
	const double s2 = sin_lat * sin_lat;
	const double c_m1 = std::pow(cos_lat, M - 1);
	const double c_m = std::pow(cos_lat, M);
	const double c_2m = std::pow(cos_lat, 2 * M);
	const double c_2m_minus2 = std::pow(cos_lat, 2 * M - 2);

	const double u_east = RADIUS_M * omega * cos_lat
		+ RADIUS_M * K * c_m1
			* (static_cast<double>(M) * s2 - c2)
			* std::cos(static_cast<double>(M) * phase_lon);
	const double v_north = -RADIUS_M * K * static_cast<double>(M) * c_m1
		* sin_lat * std::sin(static_cast<double>(M) * phase_lon);

	// Williamson TC6 height coefficients, arranged without cos^-2 terms so the
	// formula is numerically regular near the poles.
	const double A = 0.5 * omega * (2.0 * OMEGA + omega) * c2
		+ 0.25 * K * K * (
			(static_cast<double>(M) + 1.0) * c_2m * c2
			+ static_cast<double>(2 * M * M - M - 2) * c_2m
			- static_cast<double>(2 * M * M) * c_2m_minus2);
	const double B = 2.0 * (OMEGA + omega) * K
		/ static_cast<double>((M + 1) * (M + 2))
		* c_m
		* (static_cast<double>(M * M + 2 * M + 2)
			- static_cast<double>((M + 1) * (M + 1)) * c2);
	const double C = 0.25 * K * K * c_2m
		* ((static_cast<double>(M) + 1.0) * c2 - (static_cast<double>(M) + 2.0));

	RossbyReference out;
	out.depth = H0 + (RADIUS_M * RADIUS_M / GRAVITY)
		* (A + B * std::cos(static_cast<double>(M) * phase_lon)
			+ C * std::cos(static_cast<double>(2 * M) * phase_lon));

	Vec3d east = cross(Vec3d{0.0, 1.0, 0.0}, p);
	if (length(east) < 1.0e-12) east = Vec3d{1.0, 0.0, 0.0};
	else east = normalized(east);
	Vec3d north = Vec3d{0.0, 1.0, 0.0} - p * dot(Vec3d{0.0, 1.0, 0.0}, p);
	if (length(north) < 1.0e-12) north = Vec3d{0.0, 0.0, 1.0};
	else north = normalized(north);
	out.velocity = east * u_east + north * v_north;
	return out;
}

static void run_rossby_haurwitz_gate() {
	constexpr int N = 16;
	CubedSphereGrid grid(N, RADIUS_M);
	ShallowWaterCGrid sw(grid, GRAVITY, OMEGA, Vec3d{0.0, 1.0, 0.0});
	auto state = sw.make_uniform_state(8000.0);
	for (int c = 0; c < grid.cell_count(); ++c) {
		state.depth_m[c] = rossby_haurwitz_reference(grid.cell(c).center, 0.0).depth;
	}
	state.edge_normal_mps = sw.topology().sample_edge_normal_velocity(
		[](const Vec3d &p) -> Vec3d {
			return rossby_haurwitz_reference(p, 0.0).velocity;
		});

	const double mass0 = sw.total_volume_m3(state);
	const double energy0 = sw.total_energy(state);
	const double initial_min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	const double initial_max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	double initial_max_speed = 0.0;
	for (double u : state.edge_normal_mps) initial_max_speed = std::max(initial_max_speed, std::abs(u));

	// Ten planetary rotations are long enough for a grid-aligned defect to grow
	// visibly but stay inside the accepted short-time usefulness of SWTC6.
	const double duration = 10.0 * ROTATION_PERIOD_S;
	double elapsed = 0.0;
	int steps = 0;
	double worst_cfl = 0.0;
	while (elapsed < duration) {
		const double request = std::min(900.0, duration - elapsed);
		const auto diag = sw.step(state, request, 0.33);
		require(diag.accepted_dt_s > 0.0, "Rossby-Haurwitz timestep collapsed");
		require(diag.min_depth_m > 0.0, "Rossby-Haurwitz produced non-positive depth");
		worst_cfl = std::max(worst_cfl, diag.max_wave_courant);
		elapsed += diag.accepted_dt_s;
		++steps;
		require(steps < 30000, "Rossby-Haurwitz used excessive timesteps");
	}

	const double mass_error = std::abs(sw.total_volume_m3(state) - mass0) / mass0;
	const double energy_error = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	const auto cell_velocity = sw.reconstruct_cell_velocity(state);
	const auto cell_vorticity = sw.reconstruct_cell_relative_vorticity(state);

	long double reference_error2 = 0.0L;
	long double reference_scale2 = 0.0L;
	long double seam_error2 = 0.0L;
	long double seam_area = 0.0L;
	long double interior_error2 = 0.0L;
	long double interior_area = 0.0L;
	long double vort_sum = 0.0L;
	long double vort_abs = 0.0L;
	std::array<long double, CubedSphereGrid::FACE_COUNT> face_error_sum{};
	std::array<long double, CubedSphereGrid::FACE_COUNT> face_area{};
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto ref = rossby_haurwitz_reference(grid.cell(c).center, duration);
		const double dh = state.depth_m[c] - ref.depth;
		const long double area = grid.cell(c).area_m2;
		reference_error2 += static_cast<long double>(dh * dh) * area;
		const double ref_anomaly = ref.depth - 8000.0;
		reference_scale2 += static_cast<long double>(ref_anomaly * ref_anomaly) * area;
		const auto addr = grid.address(c);
		const bool seam = addr.i < 2 || addr.i >= N - 2 || addr.j < 2 || addr.j >= N - 2;
		if (seam) {
			seam_error2 += static_cast<long double>(dh * dh) * area;
			seam_area += area;
		} else {
			interior_error2 += static_cast<long double>(dh * dh) * area;
			interior_area += area;
		}
		face_error_sum[addr.face] += static_cast<long double>(dh) * area;
		face_area[addr.face] += area;
		vort_sum += static_cast<long double>(cell_vorticity[c]) * area;
		vort_abs += std::abs(static_cast<long double>(cell_vorticity[c])) * area;
	}
	const double normalized_reference_l2 = std::sqrt(static_cast<double>(
		reference_error2 / std::max(reference_scale2, 1.0L)));
	const double seam_rms = std::sqrt(static_cast<double>(seam_error2 / seam_area));
	const double interior_rms = std::sqrt(static_cast<double>(interior_error2 / interior_area));
	const double seam_imprint_ratio = seam_rms / std::max(interior_rms, 1.0e-12);
	const double vorticity_residual = static_cast<double>(
		std::abs(vort_sum) / std::max(vort_abs, 1.0L));

	double face_mean_min = std::numeric_limits<double>::infinity();
	double face_mean_max = -std::numeric_limits<double>::infinity();
	for (int f = 0; f < CubedSphereGrid::FACE_COUNT; ++f) {
		const double mean = static_cast<double>(face_error_sum[f] / face_area[f]);
		face_mean_min = std::min(face_mean_min, mean);
		face_mean_max = std::max(face_mean_max, mean);
	}
	const double face_mean_spread = face_mean_max - face_mean_min;

	const double final_min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	const double final_max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	double final_max_speed = 0.0;
	for (double u : state.edge_normal_mps) final_max_speed = std::max(final_max_speed, std::abs(u));

	std::cout << "Rossby-Haurwitz diagnostics\n"
		<< "  steps: " << steps << "\n"
		<< "  worst CFL: " << worst_cfl << "\n"
		<< "  mass error: " << mass_error << "\n"
		<< "  energy error: " << energy_error << "\n"
		<< "  normalized advected-reference L2: " << normalized_reference_l2 << "\n"
		<< "  seam/interior RMS error ratio: " << seam_imprint_ratio << "\n"
		<< "  six-face mean-error spread m: " << face_mean_spread << "\n"
		<< "  global vorticity residual: " << vorticity_residual << "\n"
		<< "  depth range initial/final m: " << initial_min_depth << ".." << initial_max_depth
		<< " / " << final_min_depth << ".." << final_max_depth << "\n"
		<< "  max edge speed initial/final m/s: " << initial_max_speed << " / " << final_max_speed << "\n";

	require(mass_error < 2e-10, "Rossby-Haurwitz mass budget drifted");
	require(energy_error < 0.025, "Rossby-Haurwitz energy drift is excessive");
	require(vorticity_residual < 2e-12, "Rossby-Haurwitz global vorticity does not close");
	require(final_min_depth > 0.25 * initial_min_depth, "Rossby-Haurwitz trough collapsed");
	require(final_max_depth < 2.0 * initial_max_depth, "Rossby-Haurwitz height grew without bound");
	require(final_max_speed < 3.0 * initial_max_speed, "Rossby-Haurwitz wind grew without bound");
	require(normalized_reference_l2 < 0.85, "Rossby-Haurwitz lost its large-scale wave structure");
	require(seam_imprint_ratio < 2.5, "cube seams dominate Rossby-Haurwitz error");
}

int main() {
	try {
		const ErrorPair coarse = run_williamson_tc2(12);
		const ErrorPair fine = run_williamson_tc2(24);
		std::cout << "Williamson TC2 convergence\n"
			<< "  N12 height/velocity error: " << coarse.height << " / " << coarse.velocity << "\n"
			<< "  N24 height/velocity error: " << fine.height << " / " << fine.velocity << "\n"
			<< "  N12 mass/energy error: " << coarse.mass << " / " << coarse.energy << "\n"
			<< "  N24 mass/energy error: " << fine.mass << " / " << fine.energy << "\n";

		require(coarse.mass < 2e-10 && fine.mass < 2e-10, "TC2 mass budget drifted");
		require(coarse.energy < 0.01 && fine.energy < 0.01, "TC2 energy drift is excessive");
		require(fine.height < coarse.height * 0.90, "TC2 height error does not converge with resolution");
		require(fine.velocity < coarse.velocity * 0.90, "TC2 velocity error does not converge with resolution");
		require(fine.height < 0.30, "TC2 fine-grid height error is excessive");
		require(fine.velocity < 0.30, "TC2 fine-grid velocity error is excessive");

		run_rossby_haurwitz_gate();
		std::cout << "Standard spherical shallow-water gates PASS\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "Standard spherical shallow-water gates FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
