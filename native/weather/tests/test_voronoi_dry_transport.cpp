#include "voronoi_dry_transport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiDryTransport;
using asterra::weather::dot;
using asterra::weather::normalized;

namespace {
constexpr double R = 3500000.0;
constexpr double G = 9.80665;
constexpr double PS = 110000.0;
constexpr double T = 288.0;
constexpr double U = 42.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double streamfunction(const Vec3d &p, const Vec3d &axis, double speed) {
	return speed * R * dot(axis, p);
}

void set_divergence_free_wind(const GeodesicVoronoiGrid &grid,
		VoronoiDryTransport::State &state, const Vec3d &axis) {
	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		const double level_speed = U * (0.72 + 0.28 * static_cast<double>(k)
			/ static_cast<double>(VoronoiDryTransport::LEVELS - 1));
		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			const double psi_a = streamfunction(grid.vertex(edge.vertex_a).center, axis, level_speed);
			const double psi_b = streamfunction(grid.vertex(edge.vertex_b).center, axis, level_speed);
			state.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
				= -(psi_b - psi_a) / edge.edge_length_m;
		}
	}
}

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max(std::abs(a), 1.0);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(8, R);
		VoronoiDryTransport transport(grid);
		require(VoronoiDryTransport::LEVELS == 30, "dry transport is not 30-level");
		require(transport.top_pressure_pa() > 5000.0 && transport.top_pressure_pa() < 10000.0,
			"dry transport model-top pressure is outside the intended ~20 km range");

		// Mass-form hydrostatic reference. Pressure is reconstructed downward from
		// the fixed model top using dp = g dm, so surface pressure is a diagnostic
		// consequence of prognostic dry mass rather than an independently forced
		// scalar. The isothermal reference must also recover T and the hypsometric
		// identity at every level.
		auto rest = transport.make_isothermal_reference(PS, T);
		const auto hydro = transport.diagnose_hydrostatic(rest);
		double max_surface_pressure_error = 0.0;
		double max_layer_mass_pressure_error = 0.0;
		double max_temperature_error = 0.0;
		double max_hydrostatic_residual = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			max_surface_pressure_error = std::max(max_surface_pressure_error,
				relative_error(hydro.surface_pressure_pa[static_cast<size_t>(c)], PS));
			const int top_i = VoronoiDryTransport::LEVELS * grid.cell_count() + c;
			require(relative_error(hydro.interface_pressure_pa[static_cast<size_t>(top_i)],
				transport.top_pressure_pa()) < 2e-15,
				"mass-form hydrostatic model-top pressure changed");
			for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				const int il = k * grid.cell_count() + c;
				const int iu = (k + 1) * grid.cell_count() + c;
				const double pl = hydro.interface_pressure_pa[static_cast<size_t>(il)];
				const double pu = hydro.interface_pressure_pa[static_cast<size_t>(iu)];
				const double mass_from_pressure = (pl - pu) / G;
				const double mass = rest.layer_mass_kg_m2[static_cast<size_t>(i)];
				require(pl > pu && pu > 0.0, "mass-form interface pressure is not monotone");
				max_layer_mass_pressure_error = std::max(max_layer_mass_pressure_error,
					relative_error(mass_from_pressure, mass));
				max_temperature_error = std::max(max_temperature_error,
					std::abs(hydro.temperature_k[static_cast<size_t>(i)] - T));
				const double expected_dphi = 287.05 * hydro.temperature_k[static_cast<size_t>(i)]
					* std::log(pl / pu);
				const double actual_dphi = hydro.interface_geopotential[static_cast<size_t>(iu)]
					- hydro.interface_geopotential[static_cast<size_t>(il)];
				max_hydrostatic_residual = std::max(max_hydrostatic_residual,
					relative_error(actual_dphi, expected_dphi));
			}
		}
		require(max_surface_pressure_error < 5e-14,
			"prognostic dry mass does not reconstruct reference surface pressure");
		require(max_layer_mass_pressure_error < 5e-13,
			"mass-form dp/g identity failed");
		require(max_temperature_error < 5e-11,
			"mass-form isothermal reference changed temperature with height");
		require(max_hydrostatic_residual < 5e-13,
			"mass-form hypsometric hydrostatic identity failed");

		const auto rest_pg = transport.pressure_gradient_acceleration(rest, hydro);
		double max_rest_pressure_accel = 0.0;
		for (double a : rest_pg) max_rest_pressure_accel = std::max(max_rest_pressure_accel, std::abs(a));
		require(max_rest_pressure_accel < 2e-12,
			"uniform mass-form hydrostatic atmosphere has a pressure-gradient source");

		// Zero-flow hydrostatic reference is an exact no-source state. This catches
		// accidental state clipping and roundoff changes from RK recombination.
		const auto rest_before = rest;
		const auto rest_diag = transport.step(rest, 21600.0, 0.45);
		require(rest.layer_mass_kg_m2 == rest_before.layer_mass_kg_m2,
			"zero-flow dry layer mass changed");
		require(rest.theta_mass_kg_k_m2 == rest_before.theta_mass_kg_k_m2,
			"zero-flow dry theta mass changed");
		require(rest_diag.relative_dry_mass_error == 0.0,
			"zero-flow dry mass diagnostic changed");

		const Vec3d axis = normalized(Vec3d{0.31, 0.89, -0.335});

		// A streamfunction-defined edge wind is discretely divergence-free. Every
		// level starts horizontally uniform, so donor-cell fluxes must leave both
		// dry mass and mass-weighted theta uniform while crossing the complete
		// spherical Voronoi topology.
		auto uniform = transport.make_isothermal_reference(PS, T);
		set_divergence_free_wind(grid, uniform, axis);
		const auto uniform_before = uniform;
		const auto uniform_diag = transport.step(uniform, 7200.0, 0.40);
		require(uniform_diag.accepted_dt_s > 0.0, "uniform dry transport rejected its timestep");
		require(uniform_diag.max_courant <= 0.4000000001,
			"uniform dry transport exceeded requested CFL");
		double max_uniform_mass_relative = 0.0;
		double max_uniform_theta_relative = 0.0;
		for (size_t i = 0; i < uniform.layer_mass_kg_m2.size(); ++i) {
			max_uniform_mass_relative = std::max(max_uniform_mass_relative,
				relative_error(uniform.layer_mass_kg_m2[i], uniform_before.layer_mass_kg_m2[i]));
			max_uniform_theta_relative = std::max(max_uniform_theta_relative,
				relative_error(uniform.theta_mass_kg_k_m2[i], uniform_before.theta_mass_kg_k_m2[i]));
		}
		require(max_uniform_mass_relative < 5e-13,
			"divergence-free wind changed horizontally uniform dry mass");
		require(max_uniform_theta_relative < 5e-13,
			"divergence-free wind changed horizontally uniform theta mass");

		// Advect a smooth warm anomaly with level-dependent wind. The mass field
		// remains positive and the globally integrated dry mass and theta mass must
		// close even though the thermodynamic pattern is moving between cells.
		auto advected = transport.make_isothermal_reference(PS, T);
		set_divergence_free_wind(grid, advected, axis);
		const Vec3d anomaly_center = normalized(Vec3d{-0.55, 0.28, 0.79});
		for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
			const double vertical_factor = std::exp(-static_cast<double>(k) / 18.0);
			for (int c = 0; c < grid.cell_count(); ++c) {
				const int i = k * grid.cell_count() + c;
				const double mu = dot(grid.cell(c).center, anomaly_center);
				const double bell = std::clamp((mu - 0.72) / 0.28, 0.0, 1.0);
				const double theta0 = advected.theta_mass_kg_k_m2[static_cast<size_t>(i)]
					/ advected.layer_mass_kg_m2[static_cast<size_t>(i)];
				const double theta = theta0 + 10.0 * vertical_factor
					* 0.5 * (1.0 - std::cos(3.14159265358979323846 * bell));
				advected.theta_mass_kg_k_m2[static_cast<size_t>(i)]
					= advected.layer_mass_kg_m2[static_cast<size_t>(i)] * theta;
			}
		}

		const double mass0 = transport.total_dry_mass_kg(advected);
		const double theta_mass0 = transport.total_theta_mass_kg_k(advected);
		const double duration = 12.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		bool saw_cfl_reduction = false;
		double max_cfl = 0.0;
		double min_mass = 1e300;
		double min_theta = 1e300;
		while (elapsed < duration) {
			const double request = std::min(7200.0, duration - elapsed);
			const auto diag = transport.step(advected, request, 0.40);
			require(diag.accepted_dt_s > 0.0, "advected dry transport timestep collapsed");
			require(diag.max_courant <= 0.4000000001,
				"advected dry transport exceeded CFL target");
			if (diag.accepted_dt_s < request * 0.999999) saw_cfl_reduction = true;
			max_cfl = std::max(max_cfl, diag.max_courant);
			min_mass = std::min(min_mass, diag.min_layer_mass_kg_m2);
			min_theta = std::min(min_theta, diag.min_potential_temperature_k);
			elapsed += diag.accepted_dt_s;
			++steps;
			require(steps < 1000, "dry transport used excessive timesteps");
		}

		const double mass_error = relative_error(transport.total_dry_mass_kg(advected), mass0);
		const double theta_mass_error = relative_error(
			transport.total_theta_mass_kg_k(advected), theta_mass0);
		require(saw_cfl_reduction, "dry transport CFL controller was not exercised");
		require(mass_error < 2e-12, "dry transport drifted in total dry mass");
		require(theta_mass_error < 2e-12,
			"dry transport drifted in mass-weighted thermodynamics");
		require(min_mass > 0.0, "dry transport produced non-positive layer mass");
		require(min_theta > 150.0, "dry transport produced invalid potential temperature");

		// The transported conservative state must remain diagnosable as a valid
		// hydrostatic column. A warm anomaly should produce a finite non-zero
		// hydrostatic pressure/geopotential force without requiring a free pressure
		// prognostic variable.
		const auto adv_hydro = transport.diagnose_hydrostatic(advected);
		for (int c = 0; c < grid.cell_count(); ++c) {
			require(std::isfinite(adv_hydro.surface_pressure_pa[static_cast<size_t>(c)])
				&& adv_hydro.surface_pressure_pa[static_cast<size_t>(c)] > transport.top_pressure_pa(),
				"transported dry state produced invalid surface pressure");
			for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				const int il = k * grid.cell_count() + c;
				const int iu = (k + 1) * grid.cell_count() + c;
				require(adv_hydro.interface_pressure_pa[static_cast<size_t>(il)]
					> adv_hydro.interface_pressure_pa[static_cast<size_t>(iu)],
					"transported dry state lost pressure monotonicity");
				require(std::isfinite(adv_hydro.temperature_k[static_cast<size_t>(i)])
					&& adv_hydro.temperature_k[static_cast<size_t>(i)] > 150.0,
					"transported dry state produced invalid temperature");
			}
		}
		const auto adv_pg = transport.pressure_gradient_acceleration(advected, adv_hydro);
		double max_adv_pressure_accel = 0.0;
		for (double a : adv_pg) {
			require(std::isfinite(a), "transported dry state produced non-finite pressure acceleration");
			max_adv_pressure_accel = std::max(max_adv_pressure_accel, std::abs(a));
		}
		require(max_adv_pressure_accel > 1e-7,
			"warm dry anomaly produced no hydrostatic horizontal pressure force");

		std::cout << "VoronoiDryTransport PASS\n"
			<< "  cells/edges/levels: " << grid.cell_count() << "/" << grid.edge_count()
			<< "/" << VoronoiDryTransport::LEVELS << "\n"
			<< "  mass-form ps/dp/T/hydro residuals: " << max_surface_pressure_error << "/"
			<< max_layer_mass_pressure_error << "/" << max_temperature_error << "/"
			<< max_hydrostatic_residual << "\n"
			<< "  rest/max warm pressure acceleration: " << max_rest_pressure_accel
			<< "/" << max_adv_pressure_accel << "\n"
			<< "  uniform max relative mass/theta error: " << max_uniform_mass_relative
			<< "/" << max_uniform_theta_relative << "\n"
			<< "  12h steps/max CFL: " << steps << "/" << max_cfl << "\n"
			<< "  dry/theta-mass drift: " << mass_error << "/" << theta_mass_error << "\n"
			<< "  min layer mass/theta: " << min_mass << "/" << min_theta << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryTransport FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
