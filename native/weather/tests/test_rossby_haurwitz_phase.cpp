#include "voronoi_shallow_water.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiShallowWater;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::normalized;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double A = 6.37122e6;
constexpr double G = 9.80616;
constexpr double OMEGA = 7.292e-5;
constexpr double W = 7.848e-6;
constexpr double K = 7.848e-6;
constexpr int R = 4;
constexpr double H0 = 8000.0;
constexpr double DAY = 86400.0;
const Vec3d NORTH{0.0, 1.0, 0.0};

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

std::pair<double, double> lat_lon(const Vec3d &p) {
	return {std::asin(std::clamp(p.y, -1.0, 1.0)), std::atan2(p.z, p.x)};
}

Vec3d east(const Vec3d &p) {
	const Vec3d v = cross(p, NORTH);
	const double n2 = dot(v, v);
	return n2 > 1.0e-24 ? v / std::sqrt(n2) : Vec3d{0.0, 0.0, 1.0};
}

Vec3d north(const Vec3d &p) { return normalized(cross(east(p), p)); }

double coef_a(double lat) {
	const double c = std::cos(lat), c2 = c * c;
	const double c2r = std::pow(c, 2 * R);
	const double c2rm2 = std::pow(c, 2 * R - 2);
	return 0.5 * W * (2.0 * OMEGA + W) * c2
		+ 0.25 * K * K * (c2r * ((R + 1.0) * c2 + 2.0 * R * R - R - 2.0)
			- 2.0 * R * R * c2rm2);
}

double coef_b(double lat) {
	const double c = std::cos(lat), c2 = c * c;
	return 2.0 * (OMEGA + W) * K / ((R + 1.0) * (R + 2.0)) * std::pow(c, R)
		* ((R * R + 2.0 * R + 2.0) - (R + 1.0) * (R + 1.0) * c2);
}

double coef_c(double lat) {
	const double c = std::cos(lat), c2 = c * c;
	return 0.25 * K * K * std::pow(c, 2 * R) * ((R + 1.0) * c2 - (R + 2.0));
}

double height(double lat, double lon) {
	return (G * H0 + A * A * (coef_a(lat) + coef_b(lat) * std::cos(R * lon)
		+ coef_c(lat) * std::cos(2.0 * R * lon))) / G;
}

Vec3d velocity(const Vec3d &p, double lon) {
	const double lat = lat_lon(p).first;
	const double c = std::cos(lat), s = std::sin(lat);
	const double crm1 = std::pow(c, R - 1);
	const double u = A * W * c + A * K * crm1 * (R * s * s - c * c) * std::cos(R * lon);
	const double v = -A * K * R * crm1 * s * std::sin(R * lon);
	return east(p) * u + north(p) * v;
}

// Exact MPAS TC6 streamfunction. MPAS initializes C-grid normal wind from
// dual-edge differences of this field rather than point-sampling the analytic
// velocity. That makes the initial wind discretely divergence-free.
double streamfunction(const Vec3d &p, double longitude_shift) {
	const auto [lat, lon] = lat_lon(p);
	const double shifted_lon = lon - longitude_shift;
	return -A * A * W * std::sin(lat)
		+ A * A * K * std::pow(std::cos(lat), R) * std::sin(lat)
			* std::cos(R * shifted_lon);
}

double discrete_edge_velocity(const GeodesicVoronoiGrid &grid, int edge_id,
		double longitude_shift) {
	const auto &edge = grid.edge(edge_id);
	const double psi_a = streamfunction(grid.vertex(edge.vertex_a).center, longitude_shift);
	const double psi_b = streamfunction(grid.vertex(edge.vertex_b).center, longitude_shift);
	const double raw = -(psi_b - psi_a) / edge.edge_length_m;
	// Our global edge orientation follows sorted primal cell IDs rather than
	// MPAS's right-handed verticesOnEdge convention. Recover the sign from the
	// actual orthogonal primal/dual geometry.
	const double handedness = dot(cross(edge.midpoint, edge.tangent_a_to_b), edge.normal_a_to_b);
	require(std::abs(handedness) > 0.99, "TC6 primal/dual edge handedness is not orthogonal");
	return raw * (handedness >= 0.0 ? 1.0 : -1.0);
}

double expected_phase_speed() {
	return (R * (R + 3.0) * W - 2.0 * OMEGA) / ((R + 1.0) * (R + 2.0));
}

double potential_enstrophy(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater &sw, const VoronoiShallowWater::State &state) {
	const auto q = sw.reconstruct_vertex_potential_vorticity(state);
	long double z = 0.0L;
	for (int v = 0; v < grid.vertex_count(); ++v) {
		const auto &vertex = grid.vertex(v);
		long double weighted_h = 0.0L;
		for (int k = 0; k < 3; ++k) weighted_h += static_cast<long double>(vertex.kite_area_m2[k])
			* state.depth_m[static_cast<size_t>(vertex.cells[k])];
		const long double hv = weighted_h / vertex.dual_area_m2;
		const long double qv = q[static_cast<size_t>(v)];
		z += 0.5L * vertex.dual_area_m2 * hv * qv * qv;
	}
	return static_cast<double>(z);
}

struct Comparison {
	double correlation = -2.0;
	double height_nrmse = std::numeric_limits<double>::infinity();
	double phase = 0.0;
};

Comparison compare_height(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater::State &state, double phase) {
	long double area_sum = 0.0L, model_sum = 0.0L, ref_sum = 0.0L;
	std::vector<double> ref(static_cast<size_t>(grid.cell_count()));
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		ref[static_cast<size_t>(c)] = height(lat, lon - phase);
		const long double area = grid.cell(c).area_m2;
		area_sum += area;
		model_sum += area * state.depth_m[static_cast<size_t>(c)];
		ref_sum += area * ref[static_cast<size_t>(c)];
	}
	const double mm = static_cast<double>(model_sum / area_sum);
	const double rm = static_cast<double>(ref_sum / area_sum);
	long double err2 = 0.0L, ref2 = 0.0L, cov = 0.0L, mv = 0.0L, rv = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const long double area = grid.cell(c).area_m2;
		const double ma = state.depth_m[static_cast<size_t>(c)] - mm;
		const double ra = ref[static_cast<size_t>(c)] - rm;
		const double err = state.depth_m[static_cast<size_t>(c)] - ref[static_cast<size_t>(c)];
		err2 += area * err * err;
		ref2 += area * ra * ra;
		cov += area * ma * ra;
		mv += area * ma * ma;
		rv += area * ra * ra;
	}
	Comparison out;
	out.phase = phase;
	out.height_nrmse = std::sqrt(static_cast<double>(err2 / std::max(ref2, 1.0L)));
	out.correlation = static_cast<double>(cov / std::sqrt(std::max(mv * rv, 1.0L)));
	return out;
}

Comparison best_phase_fit(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater::State &state, double expected_phase) {
	Comparison best;
	const double half_period = PI / R;
	constexpr int SAMPLES = 241;
	for (int i = 0; i < SAMPLES; ++i) {
		const double offset = -half_period + 2.0 * half_period * i / double(SAMPLES - 1);
		const Comparison c = compare_height(grid, state, expected_phase + offset);
		if (c.correlation > best.correlation) best = c;
	}
	return best;
}

struct Result {
	double expected_phase_corr = 0.0;
	double best_phase_corr = 0.0;
	double expected_height_nrmse = 0.0;
	double best_height_nrmse = 0.0;
	double velocity_nrmse = 0.0;
	double continuous_velocity_nrmse = 0.0;
	double phase_speed_error = 0.0;
	double initial_divergence_rms = 0.0;
	double mass_error = 0.0;
	double energy_error = 0.0;
	double enstrophy_error = 0.0;
	double pentagon_ratio = 0.0;
	double min_depth = 0.0;
	double max_depth = 0.0;
	double max_edge_speed = 0.0;
	int steps = 0;
};

Result run_case(int frequency, double days) {
	GeodesicVoronoiGrid grid(frequency, A);
	VoronoiShallowWater sw(grid, G, OMEGA, NORTH);
	auto state = sw.make_uniform_state(H0);
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		state.depth_m[static_cast<size_t>(c)] = height(lat, lon);
	}
	for (int e = 0; e < grid.edge_count(); ++e) {
		state.edge_normal_mps[static_cast<size_t>(e)] = discrete_edge_velocity(grid, e, 0.0);
	}

	long double divergence2 = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		long double volume_flux = 0.0L;
		const auto &cell = grid.cell(c);
		for (int e : cell.edges) {
			const auto &edge = grid.edge(e);
			const double outward_sign = edge.cell_a == c ? 1.0 : -1.0;
			volume_flux += outward_sign * edge.edge_length_m
				* state.edge_normal_mps[static_cast<size_t>(e)];
		}
		const double div = static_cast<double>(volume_flux / cell.area_m2);
		divergence2 += static_cast<long double>(cell.area_m2) * div * div;
	}
	Result r;
	r.initial_divergence_rms = std::sqrt(static_cast<double>(divergence2 / (4.0 * PI * A * A)));
	require(r.initial_divergence_rms < 2e-12, "TC6 MPAS streamfunction is not discretely divergence-free");

	const double mass0 = sw.total_volume_m3(state), energy0 = sw.total_energy(state);
	const double z0 = potential_enstrophy(grid, sw, state);
	const double duration = days * DAY;
	double elapsed = 0.0;
	int steps = 0;
	while (elapsed < duration) {
		const auto d = sw.step(state, std::min(900.0, duration - elapsed), 0.30);
		require(d.accepted_dt_s > 0.0, "TC6 timestep collapsed");
		require(d.min_depth_m > 0.0, "TC6 produced non-positive depth");
		elapsed += d.accepted_dt_s;
		++steps;
		require(steps < 100000, "TC6 excessive timestep count");
	}

	const double expected_phase = expected_phase_speed() * duration;
	const Comparison expected = compare_height(grid, state, expected_phase);
	const Comparison best = best_phase_fit(grid, state, expected_phase);
	long double uerr2 = 0.0L, uref2 = 0.0L;
	long double ucerr2 = 0.0L, ucref2 = 0.0L;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		const double ref = discrete_edge_velocity(grid, e, best.phase);
		const double err = state.edge_normal_mps[static_cast<size_t>(e)] - ref;
		const long double metric = edge.edge_area_m2;
		uerr2 += metric * err * err;
		uref2 += metric * ref * ref;
		const double lon = lat_lon(edge.midpoint).second;
		const double continuous_ref = dot(velocity(edge.midpoint, lon - best.phase), edge.normal_a_to_b);
		const double continuous_err = state.edge_normal_mps[static_cast<size_t>(e)] - continuous_ref;
		ucerr2 += metric * continuous_err * continuous_err;
		ucref2 += metric * continuous_ref * continuous_ref;
	}

	long double pe2 = 0.0L, pa = 0.0L, re2 = 0.0L, ra = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		const double err = state.depth_m[static_cast<size_t>(c)] - height(lat, lon - best.phase);
		bool pent = grid.cell(c).vertices.size() == 5;
		if (!pent) for (int n : grid.cell(c).neighbours) if (grid.cell(n).vertices.size() == 5) { pent = true; break; }
		const long double area = grid.cell(c).area_m2;
		if (pent) { pe2 += area * err * err; pa += area; }
		else { re2 += area * err * err; ra += area; }
	}

	r.expected_phase_corr = expected.correlation;
	r.best_phase_corr = best.correlation;
	r.expected_height_nrmse = expected.height_nrmse;
	r.best_height_nrmse = best.height_nrmse;
	r.velocity_nrmse = std::sqrt(static_cast<double>(uerr2 / std::max(uref2, 1.0L)));
	r.continuous_velocity_nrmse = std::sqrt(static_cast<double>(ucerr2 / std::max(ucref2, 1.0L)));
	r.phase_speed_error = std::abs(best.phase / duration - expected_phase_speed()) / std::abs(expected_phase_speed());
	r.mass_error = std::abs(sw.total_volume_m3(state) - mass0) / mass0;
	r.energy_error = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	r.enstrophy_error = std::abs(potential_enstrophy(grid, sw, state) - z0) / std::abs(z0);
	r.pentagon_ratio = std::sqrt(static_cast<double>(pe2 / std::max(pa, 1.0L)))
		/ std::max(std::sqrt(static_cast<double>(re2 / std::max(ra, 1.0L))), 1.0e-12);
	r.min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	r.max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	for (double u : state.edge_normal_mps) r.max_edge_speed = std::max(r.max_edge_speed, std::abs(u));
	r.steps = steps;
	return r;
}

void print(const char *name, const Result &r) {
	std::cout << name
		<< " expected/best corr=" << r.expected_phase_corr << "/" << r.best_phase_corr
		<< " expected/best hNRMSE=" << r.expected_height_nrmse << "/" << r.best_height_nrmse
		<< " uNRMSE(discrete/continuous)=" << r.velocity_nrmse << "/" << r.continuous_velocity_nrmse
		<< " phase-speed-error=" << r.phase_speed_error
		<< " init-div-rms=" << r.initial_divergence_rms
		<< " mass/energy/enstrophy=" << r.mass_error << "/" << r.energy_error << "/" << r.enstrophy_error
		<< " pentagon=" << r.pentagon_ratio
		<< " h[min,max]=" << r.min_depth << "," << r.max_depth
		<< " max|ue|=" << r.max_edge_speed << " steps=" << r.steps << "\n";
}
} // namespace

int main() {
	try {
		constexpr double DAYS = 3.0;
		const Result coarse = run_case(12, DAYS);
		const Result fine = run_case(20, DAYS);
		std::cout << "Rossby-Haurwitz MPAS-initialized diagnostics after " << DAYS << " days\n";
		print("  F12", coarse);
		print("  F20", fine);

		require(coarse.mass_error < 3e-11 && fine.mass_error < 3e-11, "TC6 mass conservation failed");
		require(coarse.energy_error < 3e-3 && fine.energy_error < 3e-3, "TC6 energy drift excessive");
		require(coarse.enstrophy_error < 6e-2 && fine.enstrophy_error < 6e-2, "TC6 enstrophy drift excessive");
		require(coarse.min_depth > 1000.0 && fine.min_depth > 1000.0, "TC6 unphysical thin layer");
		require(coarse.max_edge_speed < 250.0 && fine.max_edge_speed < 250.0, "TC6 runaway wind");
		require(coarse.best_phase_corr > 0.82 && fine.best_phase_corr > 0.88, "TC6 wave shape lost coherence");
		require(fine.phase_speed_error < 0.35, "TC6 propagation speed is too inaccurate");
		require(fine.best_height_nrmse < coarse.best_height_nrmse * 0.98, "TC6 height does not converge");
		require(fine.velocity_nrmse < coarse.velocity_nrmse * 0.98, "TC6 velocity does not converge");
		require(fine.pentagon_ratio < 4.0, "TC6 excessive pentagon mesh imprint");

		std::cout << "Rossby-Haurwitz PASS\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "Rossby-Haurwitz FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
