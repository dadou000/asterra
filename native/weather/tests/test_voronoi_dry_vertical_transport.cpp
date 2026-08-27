#include "voronoi_dry_vertical_transport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryVerticalTransport;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 288.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max(std::abs(a), 1.0);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(5, R);
		VoronoiDryVerticalTransport vertical(grid);
		auto state = vertical.make_isothermal_reference(PS, T);

		const size_t flux_count = static_cast<size_t>(VoronoiDryVerticalTransport::INTERFACES)
			* grid.cell_count();
		std::vector<double> zero_flux(flux_count, 0.0);
		const auto exact_before = state;
		const auto zero_diag = vertical.step(state, zero_flux, 14400.0, 0.45);
		require(zero_diag.accepted_dt_s == 14400.0,
			"zero vertical flux did not accept full timestep");
		require(state.layer_mass_kg_m2 == exact_before.layer_mass_kg_m2,
			"zero vertical flux changed layer mass");
		require(state.theta_mass_kg_k_m2 == exact_before.theta_mass_kg_k_m2,
			"zero vertical flux changed theta mass");
		require(state.edge_normal_mps == exact_before.edge_normal_mps,
			"zero vertical flux changed horizontal wind");

		constexpr int J = 6;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const int lower = J * grid.cell_count() + c;
			const int upper = (J + 1) * grid.cell_count() + c;
			const double lower_mass = state.layer_mass_kg_m2[static_cast<size_t>(lower)];
			const double upper_mass = state.layer_mass_kg_m2[static_cast<size_t>(upper)];
			const double lower_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(lower)] / lower_mass;
			const double upper_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(upper)] / upper_mass;
			state.theta_mass_kg_k_m2[static_cast<size_t>(lower)] = lower_mass * (lower_theta + 12.0);
			state.theta_mass_kg_k_m2[static_cast<size_t>(upper)] = upper_mass * (upper_theta - 6.0);
		}

		std::vector<double> column_mass_before(static_cast<size_t>(grid.cell_count()), 0.0);
		std::vector<double> column_theta_before(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				column_mass_before[static_cast<size_t>(c)] += state.layer_mass_kg_m2[static_cast<size_t>(i)];
				column_theta_before[static_cast<size_t>(c)] += state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			}
		}
		const auto wind_before = state.edge_normal_mps;
		const double total_mass_before = vertical.horizontal_transport().total_dry_mass_kg(state);
		const double total_theta_before = vertical.horizontal_transport().total_theta_mass_kg_k(state);

		std::vector<double> flux(flux_count, 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &p = grid.cell(c).center;
			const double magnitude = 0.28 + 0.22 * std::abs(p.x);
			flux[static_cast<size_t>(J * grid.cell_count() + c)] = p.y >= 0.0 ? magnitude : -magnitude;
		}

		const double requested_dt = 3600.0;
		const auto diag = vertical.step(state, flux, requested_dt, 0.40);
		require(diag.accepted_dt_s > 0.0, "vertical exchange timestep collapsed");
		require(diag.accepted_dt_s < requested_dt,
			"vertical exchange CFL controller was not exercised");
		require(diag.max_vertical_courant <= 0.4000000001,
			"vertical exchange exceeded CFL target");
		require(diag.min_layer_mass_kg_m2 > 0.0,
			"vertical exchange produced non-positive layer mass");
		require(diag.min_potential_temperature_k > 150.0,
			"vertical exchange produced invalid potential temperature");
		require(state.edge_normal_mps == wind_before,
			"vertical mass exchange modified horizontal wind");

		double max_column_mass_error = 0.0;
		double max_column_theta_error = 0.0;
		bool thermodynamic_state_changed = false;
		for (int c = 0; c < grid.cell_count(); ++c) {
			double column_mass_after = 0.0;
			double column_theta_after = 0.0;
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				column_mass_after += state.layer_mass_kg_m2[static_cast<size_t>(i)];
				column_theta_after += state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			}
			max_column_mass_error = std::max(max_column_mass_error,
				relative_error(column_mass_after, column_mass_before[static_cast<size_t>(c)]));
			max_column_theta_error = std::max(max_column_theta_error,
				relative_error(column_theta_after, column_theta_before[static_cast<size_t>(c)]));
			const int lower = J * grid.cell_count() + c;
			if (std::abs(state.layer_mass_kg_m2[static_cast<size_t>(lower)]
					- exact_before.layer_mass_kg_m2[static_cast<size_t>(lower)]) > 1e-8) {
				thermodynamic_state_changed = true;
			}
		}
		require(thermodynamic_state_changed,
			"non-zero vertical flux did not move atmospheric mass");
		require(max_column_mass_error < 3e-13,
			"closed vertical exchange did not conserve per-column dry mass");
		require(max_column_theta_error < 3e-13,
			"closed vertical exchange did not conserve per-column theta mass");

		const double total_mass_error = relative_error(
			vertical.horizontal_transport().total_dry_mass_kg(state), total_mass_before);
		const double total_theta_error = relative_error(
			vertical.horizontal_transport().total_theta_mass_kg_k(state), total_theta_before);
		require(total_mass_error < 3e-13,
			"closed vertical exchange drifted in global dry mass");
		require(total_theta_error < 3e-13,
			"closed vertical exchange drifted in global theta mass");

		const auto hydro = vertical.horizontal_transport().diagnose_hydrostatic(state);
		for (int c = 0; c < grid.cell_count(); ++c) {
			require(std::isfinite(hydro.surface_pressure_pa[static_cast<size_t>(c)])
				&& hydro.surface_pressure_pa[static_cast<size_t>(c)] > 7500.0,
				"vertical remap produced invalid surface pressure");
		}

		std::cout << "VoronoiDryVerticalTransport PASS\n"
			<< "  accepted/requested dt: " << diag.accepted_dt_s << "/" << requested_dt << "\n"
			<< "  max vertical CFL: " << diag.max_vertical_courant << "\n"
			<< "  max column mass/theta errors: " << max_column_mass_error
			<< "/" << max_column_theta_error << "\n"
			<< "  global mass/theta errors: " << total_mass_error
			<< "/" << total_theta_error << "\n"
			<< "  min layer mass/theta: " << diag.min_layer_mass_kg_m2
			<< "/" << diag.min_potential_temperature_k << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryVerticalTransport FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
