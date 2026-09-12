#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

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

		int north_pole_cell = -1;
		int south_pole_cell = -1;
		double max_y = -2.0;
		double min_y = 2.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double y = grid.cell(c).center.y;
			if (y > max_y) { max_y = y; north_pole_cell = c; }
			if (y < min_y) { min_y = y; south_pole_cell = c; }
		}
		require(north_pole_cell >= 0 && south_pole_cell >= 0
				&& std::abs(max_y - 1.0) < 1.0e-14
				&& std::abs(min_y + 1.0) < 1.0e-14,
			"dry dynamics regression requires exact north/south pole cells");

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

		// Exact hydrostatic rest must be a bitwise no-op for the numerical filter.
		auto filtered_rest = rest;
		const auto rest_filter = core.apply_divergence_damping(filtered_rest, 300.0);
		require(!rest_filter.applied
				&& rest_filter.rms_divergence_before_s1 == 0.0
				&& rest_filter.rms_divergence_after_s1 == 0.0
				&& filtered_rest.edge_normal_mps == rest.edge_normal_mps,
			"divergence damping modified exact hydrostatic rest");

		// The exact +/-Y pole cells must see the same balanced horizontal pressure
		// force as every other point. This directly guards against accidental
		// latitude/cos(latitude) singularities being introduced into the solver.
		const auto rest_hydro = core.transport().diagnose_hydrostatic(rest);
		const auto rest_pressure = core.transport().pressure_gradient_acceleration(rest, rest_hydro);
		double max_polar_rest_accel = 0.0;
		for (int pole : {north_pole_cell, south_pole_cell}) {
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				for (int e : grid.cell(pole).edges) {
					max_polar_rest_accel = std::max(max_polar_rest_accel,
						std::abs(rest_pressure[static_cast<size_t>(k * grid.edge_count() + e)]));
				}
			}
		}
		require(max_polar_rest_accel < 1.0e-12,
			"exact pole cells have a spurious dry pressure-gradient force");

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
		const double north_pole_speed = std::sqrt(std::max(0.0,
			dot(reconstructed[static_cast<size_t>(north_pole_cell)],
				reconstructed[static_cast<size_t>(north_pole_cell)])));
		const double south_pole_speed = std::sqrt(std::max(0.0,
			dot(reconstructed[static_cast<size_t>(south_pole_cell)],
				reconstructed[static_cast<size_t>(south_pole_cell)])));
		require(north_pole_speed < 1.0e-6 && south_pole_speed < 1.0e-6,
			"solid-body wind reconstruction is singular at an exact pole");
		require(core.total_relative_axial_angular_momentum_kg_m2_s(solid) > 0.0,
			"eastward solid-body flow has wrong angular-momentum sign");

		// Divergence damping must leave a nearly non-divergent solid-body rotation
		// essentially unchanged. This is the selectivity requirement: suppress
		// compressive fast modes without acting like broad Rayleigh drag.
		auto solid_filtered = solid;
		const auto solid_filter = core.apply_divergence_damping(solid_filtered, 300.0);
		double max_solid_filter_delta = 0.0;
		for (size_t i = 0; i < solid.edge_normal_mps.size(); ++i) {
			max_solid_filter_delta = std::max(max_solid_filter_delta,
				std::abs(solid_filtered.edge_normal_mps[i] - solid.edge_normal_mps[i]));
		}
		require(solid_filter.rms_divergence_after_s1
				<= solid_filter.rms_divergence_before_s1 * (1.0 + 1.0e-10) + 1.0e-18,
			"divergence filter amplified solid-body divergence");
		require(max_solid_filter_delta < 1.0e-3,
			"divergence filter damps non-divergent solid-body rotation too strongly");

		// A deliberately grid-scale divergent edge field must be strongly reduced
		// without touching any conservative scalar. This isolates the filter from
		// pressure dynamics and catches a sign error in +nu grad(div u).
		auto noisy = rest;
		std::vector<double> pseudo_phi(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			pseudo_phi[static_cast<size_t>(c)] = std::sin(12.9898 * (c + 1.0))
				+ 0.5 * std::sin(78.233 * (c + 0.25));
		}
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			for (int e = 0; e < grid.edge_count(); ++e) {
				const auto &edge = grid.edge(e);
				noisy.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
					= 30.0 * (pseudo_phi[static_cast<size_t>(edge.cell_b)]
						- pseudo_phi[static_cast<size_t>(edge.cell_a)]);
			}
		}
		const auto noisy_scalars = noisy.layer_mass_kg_m2;
		const auto noisy_theta = noisy.theta_mass_kg_k_m2;
		const auto noisy_filter = core.apply_divergence_damping(noisy, 300.0);
		require(noisy_filter.applied && noisy_filter.substeps >= 1,
			"grid-scale divergent field did not activate damping");
		require(noisy_filter.rms_divergence_after_s1
				< 0.995 * noisy_filter.rms_divergence_before_s1,
			"grid-scale divergent field was not measurably damped");
		require(noisy.layer_mass_kg_m2 == noisy_scalars
				&& noisy.theta_mass_kg_k_m2 == noisy_theta,
			"divergence damping modified conservative thermodynamic scalars");

		// Direct A/B pressure-pulse regression. Both cores begin with an identical
		// unbalanced cosine-bell pressure anomaly. The horizontal solver and remap
		// therefore produce the same conservative scalar state and accept the same
		// timestep; only the filtered core may reduce the final divergent wind.
		VoronoiDryCore pulse_filtered_core(grid, 9.80665, 8000.0, 7500.0,
			0.0, {0.0, 1.0, 0.0});
		VoronoiDryCore pulse_raw_core(grid, 9.80665, 8000.0, 7500.0,
			0.0, {0.0, 1.0, 0.0});
		pulse_raw_core.set_divergence_damping_strength(0.0);
		auto pulse_filtered = pulse_filtered_core.make_isothermal_reference(PS, T);
		auto pulse_raw = pulse_filtered;
		constexpr double PULSE_RADIUS = 0.55;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double angle = std::acos(std::clamp(grid.cell(c).center.x, -1.0, 1.0));
			if (angle >= PULSE_RADIUS) continue;
			const double bell = 0.5 * (1.0 + std::cos(PI * angle / PULSE_RADIUS));
			const double factor = 1.0 + 0.03 * bell;
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				pulse_filtered.layer_mass_kg_m2[i] *= factor;
				pulse_filtered.theta_mass_kg_k_m2[i] *= factor;
				pulse_raw.layer_mass_kg_m2[i] *= factor;
				pulse_raw.theta_mass_kg_k_m2[i] *= factor;
			}
		}
		const auto pulse_filtered_step = pulse_filtered_core.step(pulse_filtered, 300.0, 0.28);
		const auto pulse_raw_step = pulse_raw_core.step(pulse_raw, 300.0, 0.28);
		require(pulse_filtered_step.accepted_dt_s > 0.0
				&& pulse_raw_step.accepted_dt_s > 0.0,
			"pressure-pulse damping A/B timestep collapsed");
		require(std::abs(pulse_filtered_step.accepted_dt_s
				- pulse_raw_step.accepted_dt_s) < 1.0e-12,
			"divergence damping changed the pressure-wave CFL timestep");
		require(pulse_filtered.layer_mass_kg_m2 == pulse_raw.layer_mass_kg_m2
				&& pulse_filtered.theta_mass_kg_k_m2 == pulse_raw.theta_mass_kg_k_m2,
			"divergence damping changed pressure-pulse conservative scalars");
		const double pulse_filtered_div =
			pulse_filtered_core.rms_horizontal_divergence_s1(pulse_filtered);
		const double pulse_raw_div = pulse_raw_core.rms_horizontal_divergence_s1(pulse_raw);
		require(pulse_filtered_step.divergence_damping_applied,
			"pressure pulse did not activate automatic core divergence damping");
		require(pulse_filtered_div < 0.995 * pulse_raw_div,
			"automatic divergence damping did not reduce pressure-pulse ringing");

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
			<< "  polar rest max pressure accel: " << max_polar_rest_accel << " m/s2\n"
			<< "  polar solid-body speeds: " << north_pole_speed << "/" << south_pole_speed << " m/s\n"
			<< "  solid-body velocity reconstruction L2: " << velocity_relative_l2 << "\n"
			<< "  solid-body filter max delta: " << max_solid_filter_delta << " m/s\n"
			<< "  synthetic divergence RMS before/after: "
			<< noisy_filter.rms_divergence_before_s1 << "/"
			<< noisy_filter.rms_divergence_after_s1 << " 1/s\n"
			<< "  pressure-pulse divergence raw/filtered: "
			<< pulse_raw_div << "/" << pulse_filtered_div << " 1/s\n"
			<< "  pressure-pulse damping max accel/Courant: "
			<< pulse_filtered_step.max_divergence_damping_acceleration_mps2 << "/"
			<< pulse_filtered_step.max_divergence_damping_courant << "\n"
			<< "  3h steps: " << steps << "\n"
			<< "  3h energy drift: " << energy_drift << "\n"
			<< "  3h absolute AAM drift: " << aam_drift << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryBudgets FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
