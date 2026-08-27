#include "voronoi_dry_core.h"
#include "voronoi_dry_vertical_transport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiDryVerticalTransport;

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

void add_two_tracers(VoronoiDryCore::State &state,
		const GeodesicVoronoiGrid &grid) {
	const size_t scalar_count = static_cast<size_t>(VoronoiDryCore::LEVELS)
		* static_cast<size_t>(grid.cell_count());
	state.tracer_mass_kg_m2.assign(2, std::vector<double>(scalar_count, 0.0));
	for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
		const double vertical = static_cast<double>(k) /
			static_cast<double>(VoronoiDryCore::LEVELS - 1);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const int i = k * grid.cell_count() + c;
			const auto &p = grid.cell(c).center;
			const double q0 = 0.0025 + 0.0015 * (0.5 + 0.5 * p.x)
				+ 0.0010 * vertical;
			const double bell = std::max(0.0, 1.0 - 2.0 * (1.0 - p.z));
			const double q1 = 0.0002 + 0.0018 * bell * (1.0 - 0.65 * vertical);
			const double dry = state.layer_mass_kg_m2[static_cast<size_t>(i)];
			state.tracer_mass_kg_m2[0][static_cast<size_t>(i)] = dry * q0;
			state.tracer_mass_kg_m2[1][static_cast<size_t>(i)] = dry * q1;
		}
	}
}

std::pair<double, double> mixing_ratio_range(
		const VoronoiDryCore::State &state, size_t tracer) {
	double minimum = std::numeric_limits<double>::infinity();
	double maximum = -std::numeric_limits<double>::infinity();
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double q = state.tracer_mass_kg_m2[tracer][i]
			/ state.layer_mass_kg_m2[i];
		minimum = std::min(minimum, q);
		maximum = std::max(maximum, q);
	}
	return {minimum, maximum};
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(6, R);

		// First exercise explicit vertical donor transport and the coordinate remap.
		VoronoiDryVerticalTransport vertical(grid);
		auto column_state = vertical.make_isothermal_reference(PS, T);
		add_two_tracers(column_state, grid);
		std::vector<double> vertical_total_before(2);
		for (int t = 0; t < 2; ++t) {
			vertical_total_before[static_cast<size_t>(t)]
				= vertical.horizontal_transport().total_tracer_mass_kg(column_state, t);
		}
		const auto vertical_range0 = mixing_ratio_range(column_state, 0);
		const auto vertical_range1 = mixing_ratio_range(column_state, 1);

		std::vector<double> interface_flux(
			static_cast<size_t>(VoronoiDryVerticalTransport::INTERFACES)
				* static_cast<size_t>(grid.cell_count()), 0.0);
		constexpr int J = 8;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &p = grid.cell(c).center;
			interface_flux[static_cast<size_t>(J * grid.cell_count() + c)]
				= (p.y >= 0.0 ? 1.0 : -1.0) * (0.18 + 0.12 * std::abs(p.x));
		}
		const auto vertical_diag = vertical.step(column_state, interface_flux, 2400.0, 0.40);
		require(vertical_diag.accepted_dt_s > 0.0,
			"passive-tracer vertical exchange rejected all timesteps");
		require(vertical_diag.max_relative_tracer_mass_error < 2e-11,
			"vertical donor exchange drifted in passive tracer mass");
		const auto remap_diag = vertical.remap_to_reference_levels(column_state);
		require(remap_diag.max_relative_tracer_mass_error < 2e-11,
			"coordinate remap drifted in global passive tracer mass");
		require(remap_diag.max_column_tracer_mass_error < 2e-11,
			"coordinate remap drifted in column passive tracer mass");
		for (int t = 0; t < 2; ++t) {
			const double after = vertical.horizontal_transport().total_tracer_mass_kg(column_state, t);
			require(relative_error(after, vertical_total_before[static_cast<size_t>(t)]) < 3e-11,
				"vertical exchange + remap changed total passive tracer mass");
		}
		const auto vertical_after0 = mixing_ratio_range(column_state, 0);
		const auto vertical_after1 = mixing_ratio_range(column_state, 1);
		require(vertical_after0.first >= vertical_range0.first - 1e-12
				&& vertical_after0.second <= vertical_range0.second + 1e-12,
			"vertical tracer transport violated tracer-0 mixing-ratio bounds");
		require(vertical_after1.first >= vertical_range1.first - 1e-12
				&& vertical_after1.second <= vertical_range1.second + 1e-12,
			"vertical tracer transport violated tracer-1 mixing-ratio bounds");

		// Then exercise the complete horizontal dynamics + coordinate-remap
		// transaction. A smooth pressure anomaly generates divergent wind and forces
		// repeated remapping, while two independent tracer patterns must genuinely
		// advect without creating negative mass or changing global budgets.
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});
		auto state = core.make_isothermal_reference(PS, T);
		add_two_tracers(state, grid);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.020 * grid.cell(c).center.x);
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				state.layer_mass_kg_m2[i] *= factor;
				state.theta_mass_kg_k_m2[i] *= factor;
				for (auto &tracer : state.tracer_mass_kg_m2) tracer[i] *= factor;
			}
		}

		const auto initial_state = state;
		const auto range0 = mixing_ratio_range(state, 0);
		const auto range1 = mixing_ratio_range(state, 1);
		std::vector<double> total_before(2);
		for (int t = 0; t < 2; ++t) total_before[static_cast<size_t>(t)] = core.total_tracer_mass_kg(state, t);

		const double duration = 2.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		int remaps = 0;
		double max_step_tracer_error = 0.0;
		double max_column_tracer_error = 0.0;
		while (elapsed < duration) {
			const double request = std::min(600.0, duration - elapsed);
			const auto d = core.step(state, request, 0.28);
			require(d.accepted_dt_s > 0.0,
				"passive-tracer transactional core timestep collapsed");
			require(d.max_courant <= 0.2800000001,
				"passive-tracer transactional core exceeded CFL target");
			require(d.max_relative_tracer_mass_error < 3e-10,
				"transactional core step drifted in passive tracer mass");
			require(d.max_coordinate_column_tracer_mass_error < 2e-11,
				"coordinate remap changed a column passive-tracer budget");
			if (d.coordinate_remap_applied) ++remaps;
			max_step_tracer_error = std::max(max_step_tracer_error,
				d.max_relative_tracer_mass_error);
			max_column_tracer_error = std::max(max_column_tracer_error,
				d.max_coordinate_column_tracer_mass_error);
			elapsed += d.accepted_dt_s;
			++steps;
			require(steps < 2000, "passive-tracer core used excessive timesteps");
		}
		require(remaps > 0,
			"passive-tracer pressure-wave run never exercised coordinate remapping");

		for (int t = 0; t < 2; ++t) {
			const double after = core.total_tracer_mass_kg(state, t);
			require(relative_error(after, total_before[static_cast<size_t>(t)]) < 5e-9,
				"multi-hour dry core drifted in passive tracer mass");
		}
		const auto final_range0 = mixing_ratio_range(state, 0);
		const auto final_range1 = mixing_ratio_range(state, 1);
		require(final_range0.first >= range0.first - 2e-12
				&& final_range0.second <= range0.second + 2e-12,
			"horizontal tracer transport violated tracer-0 mixing-ratio bounds");
		require(final_range1.first >= range1.first - 2e-12
				&& final_range1.second <= range1.second + 2e-12,
			"horizontal tracer transport violated tracer-1 mixing-ratio bounds");

		long double changed = 0.0L;
		long double scale = 0.0L;
		for (size_t i = 0; i < state.tracer_mass_kg_m2[0].size(); ++i) {
			changed += std::abs(static_cast<long double>(state.tracer_mass_kg_m2[0][i])
				- static_cast<long double>(initial_state.tracer_mass_kg_m2[0][i]));
			scale += std::abs(static_cast<long double>(initial_state.tracer_mass_kg_m2[0][i]));
		}
		const double relative_field_change = static_cast<double>(changed / std::max(scale, 1.0L));
		require(relative_field_change > 1e-5,
			"passive tracer remained frozen during pressure-wave advection");

		for (const auto &tracer : state.tracer_mass_kg_m2) {
			for (double mass : tracer) {
				require(std::isfinite(mass) && mass >= 0.0,
					"passive tracer developed negative or non-finite mass");
			}
		}

		std::cout << "VoronoiPassiveTracers PASS\n"
			<< "  2h steps/remaps: " << steps << "/" << remaps << "\n"
			<< "  max step/global tracer error: " << max_step_tracer_error << "\n"
			<< "  max remap column tracer error: " << max_column_tracer_error << "\n"
			<< "  tracer-0 relative field change: " << relative_field_change << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiPassiveTracers FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
