#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;

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
		GeodesicVoronoiGrid grid(6, R);
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});

		// Exact hydrostatic rest must remain bitwise unchanged. The orchestration
		// layer detects that the coordinate is already aligned and does not remap
		// a no-source state just to manufacture floating-point noise.
		auto rest = core.make_isothermal_reference(PS, T);
		const auto rest_before = rest;
		const auto rest_diag = core.step(rest, 21600.0, 0.30);
		require(rest_diag.accepted_dt_s == 21600.0,
			"transactional dry core rejected hydrostatic rest");
		require(rest.layer_mass_kg_m2 == rest_before.layer_mass_kg_m2,
			"transactional dry core changed rest layer mass");
		require(rest.theta_mass_kg_k_m2 == rest_before.theta_mass_kg_k_m2,
			"transactional dry core changed rest theta mass");
		require(rest.edge_normal_mps == rest_before.edge_normal_mps,
			"transactional dry core accelerated rest wind");
		require(!rest_diag.coordinate_remap_applied,
			"transactional dry core remapped an already aligned rest state");
		require(rest_diag.max_coordinate_mass_fraction_error < 5e-15,
			"rest state is not aligned to reference mass coordinate");

		// Smooth pressure perturbation preserves the initial layer fractions but
		// creates a physical horizontal pressure force. Horizontal dynamics then
		// deform those mass layers; every accepted core step must conservatively
		// remap them back before publishing the new state.
		auto wave = core.make_isothermal_reference(PS, T);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.020 * grid.cell(c).center.x);
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				wave.layer_mass_kg_m2[static_cast<size_t>(i)] *= factor;
				wave.theta_mass_kg_k_m2[static_cast<size_t>(i)] *= factor;
			}
		}

		const double mass0 = core.total_dry_mass_kg(wave);
		const double theta0 = core.total_theta_mass_kg_k(wave);
		const double duration = 2.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		int remaps = 0;
		int rejected = 0;
		double max_cfl = 0.0;
		double max_fraction_error = 0.0;
		double max_column_mass_error = 0.0;
		double max_column_theta_error = 0.0;
		double max_edge_momentum_error = 0.0;
		double max_speed = 0.0;
		double min_mass = 1e300;
		double min_theta = 1e300;

		while (elapsed < duration) {
			const double request = std::min(600.0, duration - elapsed);
			const auto d = core.step(wave, request, 0.28);
			require(d.accepted_dt_s > 0.0,
				"transactional dry core timestep collapsed");
			require(d.max_courant <= 0.2800000001,
				"transactional dry core exceeded CFL target");
			require(d.relative_dry_mass_error < 3e-10,
				"transactional dry core step drifted in dry mass");
			require(d.relative_theta_mass_error < 3e-10,
				"transactional dry core step drifted in theta mass");
			require(d.max_coordinate_mass_fraction_error < 1e-14,
				"accepted dry-core state escaped reference mass coordinate");
			require(d.max_coordinate_column_mass_error < 5e-13,
				"coordinate remap changed a column dry-mass budget");
			require(d.max_coordinate_column_theta_mass_error < 5e-13,
				"coordinate remap changed a column theta-mass budget");
			require(d.max_coordinate_edge_momentum_error < 2e-11,
				"coordinate remap exceeded edge-momentum budget");
			if (d.coordinate_remap_applied) ++remaps;
			rejected += d.rejected_steps;
			max_cfl = std::max(max_cfl, d.max_courant);
			max_fraction_error = std::max(max_fraction_error,
				d.max_coordinate_mass_fraction_error);
			max_column_mass_error = std::max(max_column_mass_error,
				d.max_coordinate_column_mass_error);
			max_column_theta_error = std::max(max_column_theta_error,
				d.max_coordinate_column_theta_mass_error);
			max_edge_momentum_error = std::max(max_edge_momentum_error,
				d.max_coordinate_edge_momentum_error);
			max_speed = std::max(max_speed, d.max_speed_mps);
			min_mass = std::min(min_mass, d.min_layer_mass_kg_m2);
			min_theta = std::min(min_theta, d.min_potential_temperature_k);
			elapsed += d.accepted_dt_s;
			++steps;
			require(steps < 2000, "transactional dry core used excessive timesteps");
		}

		require(remaps > 0,
			"pressure-wave run never exercised coordinate remapping");
		const double mass_error = relative_error(core.total_dry_mass_kg(wave), mass0);
		const double theta_error = relative_error(core.total_theta_mass_kg_k(wave), theta0);
		require(mass_error < 5e-9,
			"transactional dry core drifted in total dry mass");
		require(theta_error < 5e-9,
			"transactional dry core drifted in total theta mass");
		require(core.max_coordinate_mass_fraction_error(wave) < 1e-14,
			"final dry-core state is not on reference mass coordinate");
		require(min_mass > 0.0,
			"transactional dry core produced non-positive layer mass");
		require(min_theta > 150.0,
			"transactional dry core produced invalid potential temperature");
		require(max_speed < 250.0,
			"transactional dry core developed runaway wind");

		const auto hydro = core.transport().diagnose_hydrostatic(wave);
		for (double ps : hydro.surface_pressure_pa) {
			require(std::isfinite(ps) && ps > 7500.0,
				"transactional dry core produced invalid surface pressure");
		}

		std::cout << "VoronoiDryCore PASS\n"
			<< "  2h steps/remaps/rejections: " << steps << "/" << remaps
			<< "/" << rejected << "\n"
			<< "  max CFL/wind: " << max_cfl << "/" << max_speed << "\n"
			<< "  global mass/theta drift: " << mass_error << "/" << theta_error << "\n"
			<< "  max coordinate fraction error: " << max_fraction_error << "\n"
			<< "  max remap column mass/theta error: " << max_column_mass_error
			<< "/" << max_column_theta_error << "\n"
			<< "  max remap edge momentum error: " << max_edge_momentum_error << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryCore FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
