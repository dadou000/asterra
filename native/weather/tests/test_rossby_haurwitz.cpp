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
constexpr double EARTH_RADIUS_M = 6.37122e6;
constexpr double GRAVITY = 9.80616;
constexpr double PLANET_OMEGA = 7.292e-5;
constexpr double RH_OMEGA = 7.848e-6;
constexpr double RH_K = 7.848e-6;
constexpr int RH_R = 4;
constexpr double H0 = 8000.0;
constexpr double DAY = 86400.0;
const Vec3d NORTH_AXIS{0.0, 1.0, 0.0};

void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

std::pair<double, double> lat_lon(const Vec3d &p) {
	const double lat = std::asin(std::clamp(p.y, -1.0, 1.0));
	const double lon = std::atan2(p.z, p.x);
	return {lat, lon};
}

Vec3d east_basis(const Vec3d &p) {
	Vec3d east = cross(p, NORTH_AXIS);
	const double n2 = dot(east, east);
	if (n2 < 1.0e-24) return Vec3d{0.0, 0.0, 1.0};
	return east / std::sqrt(n2);
}

Vec3d north_basis(const Vec3d &p) {
	return normalized(cross(east_basis(p), p));
}

double rh_a(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	const double c2r = std::pow(c, 2 * RH_R);
	const double c2rm2 = std::pow(c, 2 * RH_R - 2);
	return 0.5 * RH_OMEGA * (2.0 * PLANET_OMEGA + RH_OMEGA) * c2
		+ 0.25 * RH_K * RH_K * (
			c2r * ((RH_R + 1.0) * c2 + (2.0 * RH_R * RH_R - RH_R - 2.0))
			- 2.0 * RH_R * RH_R * c2rm2);
}

double rh_b(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	return (2.0 * (PLANET_OMEGA + RH_OMEGA) * RH_K
		/ ((RH_R + 1.0) * (RH_R + 2.0)))
		* std::pow(c, RH_R)
		* ((RH_R * RH_R + 2.0 * RH_R + 2.0)
			- (RH_R + 1.0) * (RH_R + 1.0) * c2);
}

double rh_c(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	return 0.25 * RH_K * RH_K * std::pow(c, 2 * RH_R)
		* ((RH_R + 1.0) * c2 - (RH_R + 2.0));
}

double rh_height(double lat, double lon) {
	const double geopotential = GRAVITY * H0 + EARTH_RADIUS_M * EARTH_RADIUS_M * (
		rh_a(lat) + rh_b(lat) * std::cos(RH_R * lon)
		+ rh_c(lat) * std::cos(2.0 * RH_R * lon));
	return geopotential / GRAVITY;
}

Vec3d rh_velocity(const Vec3d &p, double shifted_lon) {
	const auto [lat, ignored_lon] = lat_lon(p);
	(void)ignored_lon;
	const double c = std::cos(lat);
	const double s = std::sin(lat);
	const double c_r_minus_1 = std::pow(c, RH_R - 1);
	const double u_east = EARTH_RADIUS_M * RH_OMEGA * c
		+ EARTH_RADIUS_M * RH_K * c_r_minus_1
			* (RH_R * s * s - c * c) * std::cos(RH_R * shifted_lon);
	const double v_north = -EARTH_RADIUS_M * RH_K * RH_R * c_r_minus_1 * s
		* std::sin(RH_R * shifted_lon);
	return east_basis(p) * u_east + north_basis(p) * v_north;
}

double phase_speed_rad_s() {
	return (RH_R * (RH_R + 3.0) * RH_OMEGA - 2.0 * PLANET_OMEGA)
		/ ((RH_R + 1.0) * (RH_R + 2.0));
}

double potential_enstrophy(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater &sw, const VoronoiShallowWater::State &state) {
	const auto q = sw.reconstruct_vertex_potential_vorticity(state);
	long double total = 0.0L;
	for (int v = 0; v < grid.vertex_count(); ++v) {
		const auto &vertex = grid.vertex(v);
		long double weighted_h = 0.0L;
		for (int k = 0; k < 3; ++k) {
			weighted_h += static_cast<long double>(vertex.kite_area_m2[k])
				* static_cast<long double>(state.depth_m[static_cast<size_t>(vertex.cells[k])]);
		}
		const long double h_vertex = weighted_h / static_cast<long double>(vertex.dual_area_m2);
		const long double qv = q[static_cast<size_t>(v)];
		total += 0.5L * static_cast<long double>(vertex.dual_area_m2) * h_vertex * qv * qv;
	}
	return static_cast<double>(total);
}

struct Result {
	double height_nrmse = 0.0;
	double velocity_nrmse = 0.0;
	double phase_correlation = 0.0;
	double mass_error = 0.0;
	double energy_error = 0.0;
	double enstrophy_error = 0.0;
	double min_depth = 0.0;
	double max_depth = 0.0;
	double max_edge_speed = 0.0;
	double pentagon_imprint_ratio = 0.0;
	int steps = 0;
};

Result run_case(int frequency, double days) {
	GeodesicVoronoiGrid grid(frequency, EARTH_RADIUS_M);
	VoronoiShallowWater sw(grid, GRAVITY, PLANET_OMEGA, NORTH_AXIS);
	auto state = sw.make_uniform_state(H0);

	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		state.depth_m[static_cast<size_t>(c)] = rh_height(lat, lon);
	}
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		const auto [lat, lon] = lat_lon(edge.midpoint);
		(void)lat;
		state.edge_normal_mps[static_cast<size_t>(e)] = dot(
			rh_velocity(edge.midpoint, lon), edge.normal_a_to_b);
	}

	const double mass0 = sw.total_volume_m3(state);
	const double energy0 = sw.total_energy(state);
	const double enstrophy0 = potential_enstrophy(grid, sw, state);
	const double duration = days * DAY;
	double elapsed = 0.0;
	int steps = 0;
	while (elapsed < duration) {
		const double requested = std::min(900.0, duration - elapsed);
		const auto diag = sw.step(state, requested, 0.30);
		require(diag.accepted_dt_s > 0.0, "Rossby-Haurwitz timestep collapsed");
		require(diag.max_courant <= 0.3150000001, "Rossby-Haurwitz CFL escaped safety envelope");
		require(diag.min_depth_m > 0.0, "Rossby-Haurwitz produced non-positive depth");
		elapsed += diag.accepted_dt_s;
		++steps;
		require(steps < 100000, "Rossby-Haurwitz used excessive timesteps");
	}

	const double phase = phase_speed_rad_s() * duration;
	long double area_sum = 0.0L;
	long double model_mean_sum = 0.0L;
	long double reference_mean_sum = 0.0L;
	std::vector<double> reference_h(static_cast<size_t>(grid.cell_count()), 0.0);
	std::vector<double> error_h(static_cast<size_t>(grid.cell_count()), 0.0);
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		const double reference = rh_height(lat, lon - phase);
		reference_h[static_cast<size_t>(c)] = reference;
		const long double area = grid.cell(c).area_m2;
		area_sum += area;
		model_mean_sum += area * state.depth_m[static_cast<size_t>(c)];
		reference_mean_sum += area * reference;
	}
	const double model_mean = static_cast<double>(model_mean_sum / area_sum);
	const double reference_mean = static_cast<double>(reference_mean_sum / area_sum);

	long double h_error2 = 0.0L;
	long double h_reference2 = 0.0L;
	long double covariance = 0.0L;
	long double model_variance = 0.0L;
	long double reference_variance = 0.0L;
	long double pent_error2 = 0.0L;
	long double pent_area = 0.0L;
	long double regular_error2 = 0.0L;
	long double regular_area = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const double model_anomaly = state.depth_m[static_cast<size_t>(c)] - model_mean;
		const double ref_anomaly = reference_h[static_cast<size_t>(c)] - reference_mean;
		const double error = state.depth_m[static_cast<size_t>(c)] - reference_h[static_cast<size_t>(c)];
		error_h[static_cast<size_t>(c)] = error;
		const long double area = grid.cell(c).area_m2;
		h_error2 += area * error * error;
		h_reference2 += area * ref_anomaly * ref_anomaly;
		covariance += area * model_anomaly * ref_anomaly;
		model_variance += area * model_anomaly * model_anomaly;
		reference_variance += area * ref_anomaly * ref_anomaly;

		bool near_pentagon = grid.cell(c).vertices.size() == 5;
		if (!near_pentagon) {
			for (int n : grid.cell(c).neighbours) {
				if (grid.cell(n).vertices.size() == 5) {
					near_pentagon = true;
					break;
				}
			}
		}
		if (near_pentagon) {
			pent_error2 += area * error * error;
			pent_area += area;
		} else {
			regular_error2 += area * error * error;
			regular_area += area;
		}
	}

	long double u_error2 = 0.0L;
	long double u_reference2 = 0.0L;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		const auto [lat, lon] = lat_lon(edge.midpoint);
		(void)lat;
		const double reference = dot(rh_velocity(edge.midpoint, lon - phase), edge.normal_a_to_b);
		const double error = state.edge_normal_mps[static_cast<size_t>(e)] - reference;
		const long double metric = edge.edge_area_m2;
		u_error2 += metric * error * error;
		u_reference2 += metric * reference * reference;
	}

	Result r;
	r.height_nrmse = std::sqrt(static_cast<double>(h_error2 / std::max(h_reference2, 1.0L)));
	r.velocity_nrmse = std::sqrt(static_cast<double>(u_error2 / std::max(u_reference2, 1.0L)));
	r.phase_correlation = static_cast<double>(covariance /
		std::sqrt(std::max(model_variance * reference_variance, 1.0L)));
	r.mass_error = std::abs(sw.total_volume_m3(state) - mass0) / std::abs(mass0);
	r.energy_error = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	r.enstrophy_error = std::abs(potential_enstrophy(grid, sw, state) - enstrophy0)
		/ std::max(std::abs(enstrophy0), std::numeric_limits<double>::min());
	r.min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	r.max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	for (double u : state.edge_normal_mps) r.max_edge_speed = std::max(r.max_edge_speed, std::abs(u));
	const double pent_rms = std::sqrt(static_cast<double>(pent_error2 / std::max(pent_area, 1.0L)));
	const double regular_rms = std::sqrt(static_cast<double>(regular_error2 / std::max(regular_area, 1.0L)));
	r.pentagon_imprint_ratio = pent_rms / std::max(regular_rms, 1.0e-12);
	r.steps = steps;
	return r;
}

void print_result(const char *name, const Result &r) {
	std::cout << name
		<< " h/u NRMSE=" << r.height_nrmse << "/" << r.velocity_nrmse
		<< " corr=" << r.phase_correlation
		<< " mass=" << r.mass_error
		<< " energy=" << r.energy_error
		<< " enstrophy=" << r.enstrophy_error
		<< " pentagon-ratio=" << r.pentagon_imprint_ratio
		<< " min/max h=" << r.min_depth << "/" << r.max_depth
		<< " max |u_e|=" << r.max_edge_speed
		<< " steps=" << r.steps << "\n";
}

} // namespace

int main() {
	try {
		constexpr double DAYS = 5.0;
		const Result coarse = run_case(8, DAYS);
		const Result fine = run_case(12, DAYS);
		std::cout << "Rossby-Haurwitz diagnostics after " << DAYS << " days\n";
		print_result("  F8 ", coarse);
		print_result("  F12", fine);

		require(coarse.mass_error < 3e-11 && fine.mass_error < 3e-11,
			"Rossby-Haurwitz mass conservation failed");
		require(coarse.energy_error < 5e-3 && fine.energy_error < 5e-3,
			"Rossby-Haurwitz energy drift is excessive");
		require(coarse.enstrophy_error < 8e-2 && fine.enstrophy_error < 8e-2,
			"Rossby-Haurwitz potential-enstrophy drift is excessive");
		require(coarse.min_depth > 1000.0 && fine.min_depth > 1000.0,
			"Rossby-Haurwitz developed an unphysical thin layer");
		require(coarse.max_edge_speed < 250.0 && fine.max_edge_speed < 250.0,
			"Rossby-Haurwitz wind accelerated unrealistically");
		require(coarse.phase_correlation > 0.80 && fine.phase_correlation > 0.85,
			"Rossby-Haurwitz wave lost its expected phase structure");
		require(fine.height_nrmse < coarse.height_nrmse * 1.15,
			"Rossby-Haurwitz height error worsened materially with refinement");
		require(fine.velocity_nrmse < coarse.velocity_nrmse * 1.15,
			"Rossby-Haurwitz velocity error worsened materially with refinement");
		require(fine.pentagon_imprint_ratio < 5.0,
			"Rossby-Haurwitz error is excessively concentrated around pentagons");

		std::cout << "Rossby-Haurwitz PASS\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "Rossby-Haurwitz FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
