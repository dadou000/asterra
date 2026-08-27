#include "voronoi_dry_transport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryHydrostatic;
using asterra::weather::VoronoiDryTransport;

namespace {
constexpr double R = 3500000.0;
constexpr double G = 9.80665;
constexpr double P_REF = 110000.0;
constexpr double T = 288.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double actual, double expected) {
	return std::abs(actual - expected) / std::max(std::abs(expected), 1.0);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(8, R);
		VoronoiDryTransport transport(grid, G, 8000.0, 7500.0);

		// Smooth non-trivial global terrain, 0..1800 m. Keeping the field smooth
		// isolates the hydrostatic pressure-gradient balance from unresolved terrain
		// discontinuities while still exercising every orientation on the sphere.
		std::vector<double> height(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			height[static_cast<size_t>(c)] = 900.0 * (1.0 + grid.cell(c).center.x);
		}
		transport.set_surface_height_m(height);

		const auto balanced = transport.make_isothermal_terrain_balanced_reference(P_REF, T);
		const auto bd = transport.diagnose_hydrostatic(balanced);
		const auto balanced_accel = transport.pressure_gradient_acceleration(balanced, bd);

		double max_temperature_error = 0.0;
		double max_surface_pressure_error = 0.0;
		double max_surface_geopotential_error = 0.0;
		double max_balanced_accel = 0.0;
		double min_surface_pressure = std::numeric_limits<double>::infinity();
		double max_surface_pressure = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double phi_s = G * height[static_cast<size_t>(c)];
			const double expected_ps = P_REF * std::exp(
				-phi_s / (VoronoiDryHydrostatic::RD * T));
			max_surface_pressure_error = std::max(max_surface_pressure_error,
				relative_error(bd.surface_pressure_pa[static_cast<size_t>(c)], expected_ps));
			max_surface_geopotential_error = std::max(max_surface_geopotential_error,
				std::abs(bd.interface_geopotential[static_cast<size_t>(c)] - phi_s));
			min_surface_pressure = std::min(min_surface_pressure,
				bd.surface_pressure_pa[static_cast<size_t>(c)]);
			max_surface_pressure = std::max(max_surface_pressure,
				bd.surface_pressure_pa[static_cast<size_t>(c)]);
			for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				max_temperature_error = std::max(max_temperature_error,
					std::abs(bd.temperature_k[static_cast<size_t>(i)] - T));
			}
		}
		for (double a : balanced_accel) {
			max_balanced_accel = std::max(max_balanced_accel, std::abs(a));
		}

		require(max_temperature_error < 5e-11,
			"terrain-balanced isothermal reference changed temperature");
		require(max_surface_pressure_error < 5e-13,
			"terrain-balanced surface pressure is inconsistent with hydrostatic relation");
		require(max_surface_geopotential_error < 2e-11,
			"diagnosed lower-boundary geopotential does not match terrain");
		require(max_balanced_accel < 5e-11,
			"terrain-balanced isothermal atmosphere has a spurious pressure force");
		require(min_surface_pressure < max_surface_pressure,
			"terrain did not produce a surface-pressure range");

		// Deliberately put uniform surface pressure over the same terrain. Since p,
		// T and the vertical hydrostatic increments are then horizontally uniform,
		// the pressure force must reduce exactly to -grad(Phi_s) = -g grad(h).
		const auto forced = transport.make_isothermal_reference(P_REF, T);
		const auto fd = transport.diagnose_hydrostatic(forced);
		const auto forced_accel = transport.pressure_gradient_acceleration(forced, fd);
		long double accel2 = 0.0L;
		long double expected2 = 0.0L;
		double max_forced_identity_error = 0.0;
		long long samples = 0;
		for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
			for (int e = 0; e < grid.edge_count(); ++e) {
				const auto &edge = grid.edge(e);
				const double expected = -G * (
					height[static_cast<size_t>(edge.cell_b)]
					- height[static_cast<size_t>(edge.cell_a)]) / edge.center_distance_m;
				const double actual = forced_accel[
					static_cast<size_t>(k * grid.edge_count() + e)];
				const double error = actual - expected;
				max_forced_identity_error = std::max(max_forced_identity_error, std::abs(error));
				accel2 += static_cast<long double>(actual) * actual;
				expected2 += static_cast<long double>(expected) * expected;
				++samples;
			}
		}
		const double rms_accel = std::sqrt(static_cast<double>(accel2 / samples));
		const double relative_l2 = std::sqrt(static_cast<double>(
			(accel2 + expected2 - 2.0L * std::sqrt(accel2 * expected2))
			/ std::max(expected2, 1.0e-300L)));
		require(rms_accel > 1e-4,
			"uniform pressure over terrain produced no terrain pressure force");
		require(max_forced_identity_error < 5e-11,
			"terrain pressure force does not reduce to -g grad(h) for uniform columns");

		std::cout << "VoronoiDryOrography PASS\n"
			<< "  terrain height range: 0..1800 m\n"
			<< "  surface pressure range: " << min_surface_pressure
			<< ".." << max_surface_pressure << " Pa\n"
			<< "  max balanced pressure acceleration: " << max_balanced_accel << " m/s2\n"
			<< "  forced terrain RMS acceleration: " << rms_accel << " m/s2\n"
			<< "  forced identity max error: " << max_forced_identity_error << " m/s2\n"
			<< "  diagnostic relative-L2 indicator: " << relative_l2 << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryOrography FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
