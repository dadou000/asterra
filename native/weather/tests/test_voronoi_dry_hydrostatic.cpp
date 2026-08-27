#include "voronoi_dry_hydrostatic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryHydrostatic;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double R = 3500000.0;
constexpr double G = 9.80665;
constexpr double PS = 110000.0;
constexpr double T = 288.0;

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
}

int main() {
	try {
		GeodesicVoronoiGrid grid(8, R);
		VoronoiDryHydrostatic hydro(grid, G, 8000.0);
		require(VoronoiDryHydrostatic::LEVELS == 30, "dry core does not have 30 levels");
		require(hydro.top_sigma() > 0.05 && hydro.top_sigma() < 0.10,
			"dry-core top sigma is outside the intended ~20 km range");

		auto rest = hydro.make_isothermal_reference(PS, T);
		const auto d = hydro.diagnose(rest);
		double max_temperature_error = 0.0;
		double max_hydrostatic_residual = 0.0;
		double max_column_mass_error = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			double column_mass = 0.0;
			for (int k = 0; k < VoronoiDryHydrostatic::LEVELS; ++k) {
				const int si = k * grid.cell_count() + c;
				const int il = k * grid.cell_count() + c;
				const int iu = (k + 1) * grid.cell_count() + c;
				const double pl = d.interface_pressure_pa[static_cast<size_t>(il)];
				const double pu = d.interface_pressure_pa[static_cast<size_t>(iu)];
				const double tl = d.temperature_k[static_cast<size_t>(si)];
				const double phi_l = d.interface_geopotential[static_cast<size_t>(il)];
				const double phi_u = d.interface_geopotential[static_cast<size_t>(iu)];
				require(pl > pu && pu > 0.0, "interface pressure is not monotone");
				require(phi_u > phi_l, "hydrostatic geopotential is not monotone");
				max_temperature_error = std::max(max_temperature_error, std::abs(tl - T));
				const double expected_dphi = VoronoiDryHydrostatic::RD * tl * std::log(pl / pu);
				max_hydrostatic_residual = std::max(max_hydrostatic_residual,
					std::abs((phi_u - phi_l) - expected_dphi) / std::max(std::abs(expected_dphi), 1.0));
				column_mass += d.layer_mass_kg_m2[static_cast<size_t>(si)];
			}
			const double expected_column = PS * (1.0 - hydro.top_sigma()) / G;
			max_column_mass_error = std::max(max_column_mass_error,
				std::abs(column_mass - expected_column) / expected_column);
		}
		require(max_temperature_error < 2e-11, "isothermal reference changed temperature with height");
		require(max_hydrostatic_residual < 2e-13, "hypsometric hydrostatic identity failed");
		require(max_column_mass_error < 2e-13, "layer masses do not close to surface pressure");

		const auto rest_accel = hydro.pressure_gradient_acceleration(rest, d);
		double max_rest_accel = 0.0;
		for (double a : rest_accel) max_rest_accel = std::max(max_rest_accel, std::abs(a));
		require(max_rest_accel < 2e-12, "uniform hydrostatic atmosphere has a pressure-gradient source");

		// Exact isothermal pressure-gradient test. Surface pressure varies smoothly,
		// while theta is adjusted per cell so actual T is spatially uniform. At a
		// fixed sigma level Phi is then identical between cells and the primitive-
		// equation pressure force must reduce to -Rd*T*grad(ln ps).
		auto wave = rest;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double ps = PS * std::exp(0.025 * grid.cell(c).center.x);
			wave.surface_pressure_pa[static_cast<size_t>(c)] = ps;
			for (int k = 0; k < VoronoiDryHydrostatic::LEVELS; ++k) {
				const double sigma_c = std::sqrt(hydro.sigma_interfaces()[k] * hydro.sigma_interfaces()[k + 1]);
				const double pc = ps * sigma_c;
				const double theta = T * std::pow(VoronoiDryHydrostatic::P0_PA / pc,
					VoronoiDryHydrostatic::KAPPA);
				wave.potential_temperature_k[static_cast<size_t>(k * grid.cell_count() + c)] = theta;
			}
		}
		const auto wd = hydro.diagnose(wave);
		const auto accel = hydro.pressure_gradient_acceleration(wave, wd);
		long double pg_error2 = 0.0L;
		long double pg_reference2 = 0.0L;
		long double accel2 = 0.0L;
		double max_pg_absolute_error = 0.0;
		long long samples = 0;
		for (int k = 0; k < VoronoiDryHydrostatic::LEVELS; ++k) {
			for (int e = 0; e < grid.edge_count(); ++e) {
				const auto &edge = grid.edge(e);
				const double grad_ln_ps = (std::log(wave.surface_pressure_pa[static_cast<size_t>(edge.cell_b)])
					- std::log(wave.surface_pressure_pa[static_cast<size_t>(edge.cell_a)])) / edge.center_distance_m;
				const double expected = -VoronoiDryHydrostatic::RD * T * grad_ln_ps;
				const double actual = accel[static_cast<size_t>(k * grid.edge_count() + e)];
				const double error = actual - expected;
				pg_error2 += static_cast<long double>(error) * error;
				pg_reference2 += static_cast<long double>(expected) * expected;
				accel2 += static_cast<long double>(actual) * actual;
				max_pg_absolute_error = std::max(max_pg_absolute_error, std::abs(error));
				++samples;
			}
		}
		const double pg_relative_l2 = std::sqrt(static_cast<double>(
			pg_error2 / std::max(pg_reference2, std::numeric_limits<long double>::min())));
		const double rms_accel = std::sqrt(static_cast<double>(accel2 / samples));
		require(rms_accel > 1e-5, "surface-pressure perturbation produced no horizontal force");
		require(pg_relative_l2 < 2e-9,
			"sigma-coordinate isothermal pressure-gradient L2 identity failed");
		require(max_pg_absolute_error < 2e-12,
			"sigma-coordinate isothermal pressure-gradient absolute identity failed");

		long double mesh_area = 0.0L;
		for (int c = 0; c < grid.cell_count(); ++c) mesh_area += grid.cell(c).area_m2;
		const double expected_mass = static_cast<double>(mesh_area) * PS * (1.0 - hydro.top_sigma()) / G;
		const double total_mass = hydro.total_dry_air_mass_kg(rest);
		const double mass_error = std::abs(total_mass - expected_mass) / expected_mass;
		require(mass_error < 2e-13, "global dry-air mass diagnostic does not match column integral");

		std::cout << "VoronoiDryHydrostatic PASS\n"
			<< "  top sigma: " << hydro.top_sigma() << "\n"
			<< "  max T error: " << max_temperature_error << " K\n"
			<< "  hydrostatic residual: " << max_hydrostatic_residual << "\n"
			<< "  column/global mass residual: " << max_column_mass_error << "/" << mass_error << "\n"
			<< "  max rest acceleration: " << max_rest_accel << " m/s2\n"
			<< "  pressure-gradient RMS/L2/maxabs: " << rms_accel << "/"
			<< pg_relative_l2 << "/" << max_pg_absolute_error << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryHydrostatic FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
