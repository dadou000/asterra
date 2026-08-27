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
using asterra::weather::dot;

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

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

std::pair<double, double> lat_lon(const Vec3d &p) {
	return {std::asin(std::clamp(p.y, -1.0, 1.0)), std::atan2(p.z, p.x)};
}

double coef_a(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	const double c2r = std::pow(c, 2 * R);
	const double c2rm2 = std::pow(c, 2 * R - 2);
	return 0.5 * W * (2.0 * OMEGA + W) * c2
		+ 0.25 * K * K * (c2r * ((R + 1.0) * c2 + 2.0 * R * R - R - 2.0)
			- 2.0 * R * R * c2rm2);
}

double coef_b(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	return 2.0 * (OMEGA + W) * K / ((R + 1.0) * (R + 2.0)) * std::pow(c, R)
		* ((R * R + 2.0 * R + 2.0) - (R + 1.0) * (R + 1.0) * c2);
}

double coef_c(double lat) {
	const double c = std::cos(lat);
	const double c2 = c * c;
	return 0.25 * K * K * std::pow(c, 2 * R) * ((R + 1.0) * c2 - (R + 2.0));
}

double height(double lat, double lon) {
	return (G * H0 + A * A * (coef_a(lat)
		+ coef_b(lat) * std::cos(R * lon)
		+ coef_c(lat) * std::cos(2.0 * R * lon))) / G;
}

double streamfunction(const Vec3d &p, double longitude_shift) {
	const auto [lat, lon] = lat_lon(p);
	const double shifted_lon = lon - longitude_shift;
	return -A * A * W * std::sin(lat)
		+ A * A * K * std::pow(std::cos(lat), R) * std::sin(lat)
			* std::cos(R * shifted_lon);
}

double edge_velocity(const GeodesicVoronoiGrid &grid, int edge_id,
		double longitude_shift) {
	const auto &edge = grid.edge(edge_id);
	const double psi_a = streamfunction(grid.vertex(edge.vertex_a).center, longitude_shift);
	const double psi_b = streamfunction(grid.vertex(edge.vertex_b).center, longitude_shift);
	// finalize_mpas_metrics() orients vertex_a -> vertex_b consistently with
	// MPAS verticesOnEdge, so the official TC6 initialization is directly -dpsi/dv.
	return -(psi_b - psi_a) / edge.edge_length_m;
}

double barotropic_phase_speed() {
	return (R * (R + 3.0) * W - 2.0 * OMEGA) / ((R + 1.0) * (R + 2.0));
}

double potential_enstrophy(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater &sw,
		const VoronoiShallowWater::State &state) {
	const auto q = sw.reconstruct_vertex_potential_vorticity(state);
	long double total = 0.0L;
	for (int v = 0; v < grid.vertex_count(); ++v) {
		const auto &vertex = grid.vertex(v);
		long double weighted_h = 0.0L;
		for (int k = 0; k < 3; ++k) {
			weighted_h += static_cast<long double>(vertex.kite_area_m2[k])
				* state.depth_m[static_cast<size_t>(vertex.cells[k])];
		}
		const long double hv = weighted_h / vertex.dual_area_m2;
		const long double qv = q[static_cast<size_t>(v)];
		total += 0.5L * vertex.dual_area_m2 * hv * qv * qv;
	}
	return static_cast<double>(total);
}

struct Fit {
	double correlation = -2.0;
	double height_nrmse = std::numeric_limits<double>::infinity();
	double phase = 0.0;
};

Fit compare_height(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater::State &state, double phase) {
	long double area_sum = 0.0L;
	long double model_sum = 0.0L;
	long double reference_sum = 0.0L;
	std::vector<double> reference(static_cast<size_t>(grid.cell_count()));

	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		reference[static_cast<size_t>(c)] = height(lat, lon - phase);
		const long double area = grid.cell(c).area_m2;
		area_sum += area;
		model_sum += area * state.depth_m[static_cast<size_t>(c)];
		reference_sum += area * reference[static_cast<size_t>(c)];
	}
	const double model_mean = static_cast<double>(model_sum / area_sum);
	const double reference_mean = static_cast<double>(reference_sum / area_sum);

	long double err2 = 0.0L;
	long double ref2 = 0.0L;
	long double covariance = 0.0L;
	long double model_var = 0.0L;
	long double reference_var = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const long double area = grid.cell(c).area_m2;
		const double model_anomaly = state.depth_m[static_cast<size_t>(c)] - model_mean;
		const double reference_anomaly = reference[static_cast<size_t>(c)] - reference_mean;
		const double error = state.depth_m[static_cast<size_t>(c)] - reference[static_cast<size_t>(c)];
		err2 += area * error * error;
		ref2 += area * reference_anomaly * reference_anomaly;
		covariance += area * model_anomaly * reference_anomaly;
		model_var += area * model_anomaly * model_anomaly;
		reference_var += area * reference_anomaly * reference_anomaly;
	}

	Fit fit;
	fit.phase = phase;
	fit.height_nrmse = std::sqrt(static_cast<double>(err2 / std::max(ref2, 1.0L)));
	fit.correlation = static_cast<double>(covariance
		/ std::sqrt(std::max(model_var * reference_var, 1.0L)));
	return fit;
}

Fit best_phase_fit(const GeodesicVoronoiGrid &grid,
		const VoronoiShallowWater::State &state,
		double barotropic_expected_phase) {
	Fit best;
	// Search one complete equivalent phase interval for an R=4 wave. The
	// barotropic value is only the search centre/diagnostic: TC6 is not an exact
	// time-dependent solution of the full shallow-water equations.
	const double half_period = PI / R;
	constexpr int SAMPLES = 241;
	for (int i = 0; i < SAMPLES; ++i) {
		const double offset = -half_period
			+ 2.0 * half_period * static_cast<double>(i) / static_cast<double>(SAMPLES - 1);
		const Fit candidate = compare_height(grid, state, barotropic_expected_phase + offset);
		if (candidate.correlation > best.correlation) best = candidate;
	}
	return best;
}

double phase_distance(double a, double b) {
	const double wave_period = 2.0 * PI / R;
	return std::abs(std::remainder(a - b, wave_period));
}

struct Result {
	double expected_corr = 0.0;
	double best_corr = 0.0;
	double best_height_nrmse = 0.0;
	double velocity_nrmse = 0.0;
	double best_phase = 0.0;
	double barotropic_phase_speed_error = 0.0;
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
		state.edge_normal_mps[static_cast<size_t>(e)] = edge_velocity(grid, e, 0.0);
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
		const double divergence = static_cast<double>(volume_flux / cell.area_m2);
		divergence2 += static_cast<long double>(cell.area_m2) * divergence * divergence;
	}

	Result result;
	result.initial_divergence_rms = std::sqrt(static_cast<double>(
		divergence2 / (4.0 * PI * A * A)));
	require(result.initial_divergence_rms < 2e-12,
		"TC6 streamfunction initialization is not discretely divergence-free");

	const double mass0 = sw.total_volume_m3(state);
	const double energy0 = sw.total_energy(state);
	const double enstrophy0 = potential_enstrophy(grid, sw, state);
	const double duration = days * DAY;
	double elapsed = 0.0;
	while (elapsed < duration) {
		const auto diagnostics = sw.step(state, std::min(900.0, duration - elapsed), 0.30);
		require(diagnostics.accepted_dt_s > 0.0, "TC6 timestep collapsed");
		require(diagnostics.min_depth_m > 0.0, "TC6 produced non-positive depth");
		elapsed += diagnostics.accepted_dt_s;
		++result.steps;
		require(result.steps < 100000, "TC6 excessive timestep count");
	}

	const double barotropic_expected_phase = barotropic_phase_speed() * duration;
	const Fit expected = compare_height(grid, state, barotropic_expected_phase);
	const Fit best = best_phase_fit(grid, state, barotropic_expected_phase);
	result.expected_corr = expected.correlation;
	result.best_corr = best.correlation;
	result.best_height_nrmse = best.height_nrmse;
	result.best_phase = best.phase;
	result.barotropic_phase_speed_error = std::abs(best.phase / duration - barotropic_phase_speed())
		/ std::abs(barotropic_phase_speed());

	long double velocity_error2 = 0.0L;
	long double velocity_reference2 = 0.0L;
	for (int e = 0; e < grid.edge_count(); ++e) {
		const double reference = edge_velocity(grid, e, best.phase);
		const double error = state.edge_normal_mps[static_cast<size_t>(e)] - reference;
		const long double metric = grid.edge(e).edge_area_m2;
		velocity_error2 += metric * error * error;
		velocity_reference2 += metric * reference * reference;
		result.max_edge_speed = std::max(result.max_edge_speed,
			std::abs(state.edge_normal_mps[static_cast<size_t>(e)]));
	}
	result.velocity_nrmse = std::sqrt(static_cast<double>(
		velocity_error2 / std::max(velocity_reference2, 1.0L)));

	long double pentagon_error2 = 0.0L;
	long double pentagon_area = 0.0L;
	long double regular_error2 = 0.0L;
	long double regular_area = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		const auto [lat, lon] = lat_lon(grid.cell(c).center);
		const double error = state.depth_m[static_cast<size_t>(c)] - height(lat, lon - best.phase);
		bool near_pentagon = grid.cell(c).vertices.size() == 5;
		if (!near_pentagon) {
			for (int neighbour : grid.cell(c).neighbours) {
				if (grid.cell(neighbour).vertices.size() == 5) {
					near_pentagon = true;
					break;
				}
			}
		}
		const long double area = grid.cell(c).area_m2;
		if (near_pentagon) {
			pentagon_error2 += area * error * error;
			pentagon_area += area;
		} else {
			regular_error2 += area * error * error;
			regular_area += area;
		}
	}
	const double pentagon_rms = std::sqrt(static_cast<double>(
		pentagon_error2 / std::max(pentagon_area, 1.0L)));
	const double regular_rms = std::sqrt(static_cast<double>(
		regular_error2 / std::max(regular_area, 1.0L)));
	result.pentagon_ratio = pentagon_rms / std::max(regular_rms, 1.0e-12);

	result.mass_error = std::abs(sw.total_volume_m3(state) - mass0) / mass0;
	result.energy_error = std::abs(sw.total_energy(state) - energy0) / std::abs(energy0);
	result.enstrophy_error = std::abs(potential_enstrophy(grid, sw, state) - enstrophy0)
		/ std::abs(enstrophy0);
	result.min_depth = *std::min_element(state.depth_m.begin(), state.depth_m.end());
	result.max_depth = *std::max_element(state.depth_m.begin(), state.depth_m.end());
	return result;
}

void print_result(const char *name, const Result &result) {
	std::cout << name
		<< " expected/best corr=" << result.expected_corr << "/" << result.best_corr
		<< " best hNRMSE=" << result.best_height_nrmse
		<< " uNRMSE=" << result.velocity_nrmse
		<< " best phase=" << result.best_phase
		<< " barotropic-phase-speed-error=" << result.barotropic_phase_speed_error
		<< " init-div-rms=" << result.initial_divergence_rms
		<< " mass/energy/enstrophy=" << result.mass_error << "/"
		<< result.energy_error << "/" << result.enstrophy_error
		<< " pentagon=" << result.pentagon_ratio
		<< " h[min,max]=" << result.min_depth << "," << result.max_depth
		<< " max|ue|=" << result.max_edge_speed
		<< " steps=" << result.steps << "\n";
}
} // namespace

int main() {
	try {
		constexpr double DAYS = 3.0;
		const Result coarse = run_case(12, DAYS);
		const Result fine = run_case(20, DAYS);
		const double phase_resolution_difference = phase_distance(coarse.best_phase, fine.best_phase);

		std::cout << "Rossby-Haurwitz MPAS-initialized diagnostics after " << DAYS << " days\n";
		print_result("  F12", coarse);
		print_result("  F20", fine);
		std::cout << "  coarse/fine best-phase difference=" << phase_resolution_difference << " rad\n";

		require(coarse.mass_error < 3e-11 && fine.mass_error < 3e-11,
			"TC6 mass conservation failed");
		require(coarse.energy_error < 3e-3 && fine.energy_error < 3e-3,
			"TC6 energy drift excessive");
		require(coarse.enstrophy_error < 6e-2 && fine.enstrophy_error < 6e-2,
			"TC6 enstrophy drift excessive");
		require(coarse.min_depth > 1000.0 && fine.min_depth > 1000.0,
			"TC6 unphysical thin layer");
		require(coarse.max_edge_speed < 250.0 && fine.max_edge_speed < 250.0,
			"TC6 runaway wind");

		// Williamson TC6 is an exact travelling wave of the nondivergent
		// barotropic-vorticity equation, but not of the full shallow-water system.
		// Therefore do not fail the SWE solver for departing from the analytic
		// barotropic phase speed. Instead require the standard qualitative behavior:
		// a coherent eastward R=4 pattern with converged phase across resolutions.
		require(coarse.best_corr > 0.97 && fine.best_corr > 0.97,
			"TC6 wave shape lost coherence");
		require(coarse.best_height_nrmse < 0.12 && fine.best_height_nrmse < 0.12,
			"TC6 height pattern departed excessively from the travelling-wave family");
		require(fine.best_height_nrmse < coarse.best_height_nrmse * 1.10,
			"TC6 height error worsened materially with refinement");
		require(fine.velocity_nrmse < coarse.velocity_nrmse * 1.05,
			"TC6 velocity error worsened materially with refinement");
		require(coarse.best_phase > 0.05 && fine.best_phase > 0.05,
			"TC6 failed to propagate eastward");
		require(phase_resolution_difference < 0.05,
			"TC6 propagation phase is not resolution-consistent");
		require(coarse.expected_corr > 0.75 && fine.expected_corr > 0.75,
			"TC6 evolution is incompatible with the expected eastward wave regime");
		require(fine.pentagon_ratio < 4.0,
			"TC6 excessive pentagon mesh imprint");

		std::cout << "Rossby-Haurwitz PASS\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "Rossby-Haurwitz FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
