#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiDryCore;
using asterra::weather::cross;
using asterra::weather::dot;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 288.0;
constexpr double OMEGA = 2.0 * PI / (11.5 * 3600.0);

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(8, R);
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});

		// Rest-state diagnostics have simple independent checks. Relative AAM is
		// exactly zero, while absolute AAM is the dry mass weighted planetary term
		// Omega R^2 (1-mu^2) on the same fixed-radius shallow atmosphere.
		auto rest = core.make_isothermal_reference(PS, T);
		const double rest_energy = core.total_dry_energy_j(rest);
		const double rest_relative_aam =
			core.total_relative_axial_angular_momentum_kg_m2_s(rest);
		const double rest_absolute_aam =
			core.total_absolute_axial_angular_momentum_kg_m2_s(rest);
		require(std::isfinite(rest_energy) && rest_energy > 0.0,
			"dry rest energy is invalid");
		require(rest_relative_aam == 0.0,
			"zero-wind atmosphere has non-zero relative angular momentum");

		long double expected_planetary_aam = 0.0L;
		for (int c = 0; c < grid.cell_count(); ++c) {
			long double column_mass = 0.0L;
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				column_mass += rest.layer_mass_kg_m2[
					static_cast<size_t>(k * grid.cell_count() + c)];
			}
			const double mu = grid.cell(c).center.y;
			const double specific = OMEGA * R * R * std::max(0.0, 1.0 - mu * mu);
			expected_planetary_aam += static_cast<long double>(grid.cell(c).area_m2)
				* column_mass * static_cast<long double>(specific);
		}
		const double rest_aam_identity_error = relative_error(
			rest_absolute_aam, static_cast<double>(expected_planetary_aam));
		require(rest_aam_identity_error < 3e-13,
			"absolute angular momentum does not match independent planetary sum");

		// Validate cell-centred wind reconstruction against analytic solid-body
		// rotation. Prognostic values are still edge normals; reconstruction exists
		// only for diagnostics/output and must not introduce a large geometric bias.
		auto solid = rest;
		constexpr double U0 = 60.0;
		const Vec3d axis{0.0, 1.0, 0.0};
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			for (int e = 0; e < grid.edge_count(); ++e) {
				const auto &edge = grid.edge(e);
				const Vec3d analytic = cross(axis, edge.midpoint) * U0;
				solid.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
					= dot(analytic, edge.normal_a_to_b);
			}
		}
		const auto reconstructed = core.reconstruct_cell_velocity(solid, 0);
		long double velocity_error2 = 0.0L;
		long double velocity_reference2 = 0.0L;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const Vec3d expected = cross(axis, grid.cell(c).center) * U0;
			const Vec3d error = reconstructed[static_cast<size_t>(c)] - expected;
			velocity_error2 += static_cast<long double>(grid.cell(c).area_m2)
				* static_cast<long double>(dot(error, error));
			velocity_reference2 += static_cast<long double>(grid.cell(c).area_m2)
				* static_cast<long double>(dot(expected, expected));
		}
		const double velocity_relative_l2 = std::sqrt(static_cast<double>(
			velocity_error2 / std::max(velocity_reference2, 1.0L)));
		require(velocity_relative_l2 < 0.08,
			"cell-centred C-grid wind reconstruction is too inaccurate");
		require(core.total_relative_axial_angular_momentum_kg_m2_s(solid) > 0.0,
			"eastward solid-body flow has wrong angular-momentum sign");

		// Characterize conservation through coupled pressure adjustment and the
		// conservative coordinate remap. These are intentionally broad bring-up
		// gates; measured values are printed so the limits can be tightened once
		// the diagnostic has established a stable baseline.
		auto wave = core.make_isothermal_reference(PS, T);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.018 * grid.cell(c).center.x);
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				wave.layer_mass_kg_m2[static_cast<size_t>(i)] *= factor;
				wave.theta_mass_kg_k_m2[static_cast<size_t>(i)] *= factor;
			}
		}
		const double energy0 = core.total_dry_energy_j(wave);
		const double aam0 = core.total_absolute_axial_angular_momentum_kg_m2_s(wave);
		const double duration = 3.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		while (elapsed < duration) {
			const double request = std::min(600.0, duration - elapsed);
			const auto d = core.step(wave, request, 0.28);
			require(d.accepted_dt_s > 0.0,
				"dry budget characterization timestep collapsed");
			elapsed += d.accepted_dt_s;
			++steps;
			require(steps < 3000, "dry budget characterization used excessive timesteps");
		}

		const double energy1 = core.total_dry_energy_j(wave);
		const double aam1 = core.total_absolute_axial_angular_momentum_kg_m2_s(wave);
		const double energy_drift = relative_error(energy1, energy0);
		const double aam_drift = relative_error(aam1, aam0);
		require(std::isfinite(energy_drift) && energy_drift < 5e-2,
			"dry total-energy drift exceeded bring-up bound");
		require(std::isfinite(aam_drift) && aam_drift < 1e-2,
			"dry axial-angular-momentum drift exceeded bring-up bound");

		std::cout << "VoronoiDryBudgets PASS\n"
			<< "  rest total energy: " << rest_energy << " J\n"
			<< "  rest absolute AAM / identity error: " << rest_absolute_aam
			<< "/" << rest_aam_identity_error << "\n"
			<< "  solid-body velocity reconstruction L2: " << velocity_relative_l2 << "\n"
			<< "  3h steps: " << steps << "\n"
			<< "  3h energy drift: " << energy_drift << "\n"
			<< "  3h absolute AAM drift: " << aam_drift << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryBudgets FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
