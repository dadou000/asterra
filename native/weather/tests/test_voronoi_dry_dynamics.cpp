#include "voronoi_dry_dynamics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryDynamics;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 288.0;
constexpr double OMEGA = 2.0 * PI / (11.5 * 3600.0);

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max(std::abs(a), 1.0);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(8, R);
		VoronoiDryDynamics dynamics(grid, 9.80665, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});

		// Uniform hydrostatic rest must be an exact discrete no-source state.
		auto rest = dynamics.make_isothermal_reference(PS, T);
		const auto rest_before = rest;
		const auto rest_diag = dynamics.step(rest, 21600.0, 0.30);
		require(rest_diag.accepted_dt_s == 21600.0,
			"dry dynamics rest state did not accept requested timestep");
		require(rest.layer_mass_kg_m2 == rest_before.layer_mass_kg_m2,
			"dry dynamics changed rest-state layer mass");
		require(rest.theta_mass_kg_k_m2 == rest_before.theta_mass_kg_k_m2,
			"dry dynamics changed rest-state theta mass");
		require(rest.edge_normal_mps == rest_before.edge_normal_mps,
			"dry dynamics accelerated uniform hydrostatic rest");
		require(rest_diag.max_pressure_acceleration_mps2 < 2e-12,
			"dry dynamics rest state has a pressure-gradient source");

		// Smooth column-mass perturbation. Scale each column's layer masses and
		// theta masses together so potential temperature initially stays unchanged.
		// Because pressure is derived from mass, this creates a real hydrostatic
		// horizontal force without an independent pressure prognostic variable.
		auto wave = dynamics.make_isothermal_reference(PS, T);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.018 * grid.cell(c).center.x);
			for (int k = 0; k < VoronoiDryDynamics::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				wave.layer_mass_kg_m2[static_cast<size_t>(i)] *= factor;
				wave.theta_mass_kg_k_m2[static_cast<size_t>(i)] *= factor;
			}
		}

		const auto hydro0 = dynamics.transport().diagnose_hydrostatic(wave);
		const auto pressure0 = dynamics.transport().pressure_gradient_acceleration(wave, hydro0);
		double initial_max_pressure_accel = 0.0;
		for (double a : pressure0) initial_max_pressure_accel = std::max(initial_max_pressure_accel, std::abs(a));
		require(initial_max_pressure_accel > 1e-5,
			"dry column-mass perturbation produced no hydrostatic pressure force");

		const double mass0 = dynamics.total_dry_mass_kg(wave);
		const double theta_mass0 = dynamics.total_theta_mass_kg_k(wave);
		const auto first = dynamics.step(wave, 300.0, 0.28);
		require(first.accepted_dt_s > 0.0, "dry dynamics rejected pressure-wave startup");
		require(first.max_courant <= 0.2800000001,
			"dry dynamics startup exceeded CFL target");
		require(first.max_speed_mps > 1e-4,
			"hydrostatic pressure perturbation did not accelerate edge wind");
		require(first.relative_dry_mass_error < 1e-10,
			"dry dynamics startup drifted in mass");
		require(first.relative_theta_mass_error < 1e-10,
			"dry dynamics startup drifted in theta mass");

		// Integrate through many coupled pressure/transport/Coriolis updates. The
		// explicit bring-up core is deliberately CFL-conservative; this test is
		// about boundedness and exact shared-flux budgets, not climate fidelity yet.
		const double duration = 6.0 * 3600.0;
		double elapsed = first.accepted_dt_s;
		int steps = 1;
		double max_cfl = first.max_courant;
		double max_speed = first.max_speed_mps;
		double min_mass = first.min_layer_mass_kg_m2;
		double min_theta = first.min_potential_temperature_k;
		int rejected = first.rejected_steps;
		while (elapsed < duration) {
			const double request = std::min(600.0, duration - elapsed);
			const auto diag = dynamics.step(wave, request, 0.28);
			require(diag.accepted_dt_s > 0.0, "dry dynamics pressure-wave timestep collapsed");
			require(diag.max_courant <= 0.2800000001,
				"dry dynamics pressure-wave run exceeded CFL target");
			max_cfl = std::max(max_cfl, diag.max_courant);
			max_speed = std::max(max_speed, diag.max_speed_mps);
			min_mass = std::min(min_mass, diag.min_layer_mass_kg_m2);
			min_theta = std::min(min_theta, diag.min_potential_temperature_k);
			rejected += diag.rejected_steps;
			elapsed += diag.accepted_dt_s;
			++steps;
			require(steps < 5000, "dry dynamics used excessive timesteps");
		}

		const double mass_error = relative_error(dynamics.total_dry_mass_kg(wave), mass0);
		const double theta_error = relative_error(dynamics.total_theta_mass_kg_k(wave), theta_mass0);
		require(mass_error < 3e-10, "coupled dry dynamics drifted in total dry mass");
		require(theta_error < 3e-10,
			"coupled dry dynamics drifted in mass-weighted potential temperature");
		require(min_mass > 0.0, "coupled dry dynamics produced non-positive layer mass");
		require(min_theta > 150.0, "coupled dry dynamics produced invalid potential temperature");
		require(max_speed < 250.0, "coupled dry dynamics developed runaway wind");

		// Relative-vorticity circulation must still close globally on the dual
		// mesh because every oriented Delaunay segment occurs equal/opposite.
		const auto zeta = dynamics.reconstruct_vertex_relative_vorticity(wave, 0);
		long double circulation = 0.0L;
		long double absolute_circulation = 0.0L;
		for (int v = 0; v < grid.vertex_count(); ++v) {
			const long double contribution = static_cast<long double>(zeta[static_cast<size_t>(v)])
				* static_cast<long double>(grid.vertex(v).dual_area_m2);
			circulation += contribution;
			absolute_circulation += std::abs(contribution);
		}
		const double circulation_residual = static_cast<double>(
			std::abs(circulation) / std::max(absolute_circulation, 1.0L));
		require(circulation_residual < 3e-13,
			"dry dynamics dual-mesh relative-vorticity circulation does not close");

		const auto final_hydro = dynamics.transport().diagnose_hydrostatic(wave);
		for (int c = 0; c < grid.cell_count(); ++c) {
			require(std::isfinite(final_hydro.surface_pressure_pa[static_cast<size_t>(c)])
				&& final_hydro.surface_pressure_pa[static_cast<size_t>(c)] > 7500.0,
				"dry dynamics produced invalid diagnosed surface pressure");
		}

		std::cout << "VoronoiDryDynamics PASS\n"
			<< "  initial max pressure acceleration: " << initial_max_pressure_accel << " m/s2\n"
			<< "  6h steps/rejections/max CFL: " << steps << "/" << rejected << "/" << max_cfl << "\n"
			<< "  dry/theta-mass drift: " << mass_error << "/" << theta_error << "\n"
			<< "  min layer mass/theta, max wind: " << min_mass << "/" << min_theta
			<< "/" << max_speed << "\n"
			<< "  circulation residual: " << circulation_residual << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryDynamics FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
