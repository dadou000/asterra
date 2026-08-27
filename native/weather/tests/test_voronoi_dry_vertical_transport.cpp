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

std::vector<double> integrated_edge_momentum(
		const GeodesicVoronoiGrid &grid,
		const VoronoiDryVerticalTransport::State &state) {
	std::vector<double> out(static_cast<size_t>(grid.edge_count()), 0.0);
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		long double momentum = 0.0L;
		for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
			const int ia = k * grid.cell_count() + edge.cell_a;
			const int ib = k * grid.cell_count() + edge.cell_b;
			const int ie = k * grid.edge_count() + e;
			const double edge_mass = 0.5 * (
				state.layer_mass_kg_m2[static_cast<size_t>(ia)]
				+ state.layer_mass_kg_m2[static_cast<size_t>(ib)]);
			momentum += static_cast<long double>(edge_mass)
				* static_cast<long double>(state.edge_normal_mps[static_cast<size_t>(ie)]);
		}
		out[static_cast<size_t>(e)] = static_cast<double>(momentum);
	}
	return out;
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

		// Build a thermodynamic contrast around one interior interface, then impose
		// upward exchange in one hemisphere and downward exchange in the other.
		// There are no top/bottom boundary fluxes, so every individual column is a
		// closed mass/theta budget.
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

		// Coordinate-remap regression. Start from a valid reference atmosphere,
		// distort the 30 layer masses while conservatively carrying theta, and add
		// a strongly sheared vertical wind profile. The remapper must restore the
		// configured pressure-coordinate mass fractions without changing any
		// column budget or the vertically integrated edge-mass-weighted momentum.
		auto remap_state = vertical.make_isothermal_reference(PS, T);
		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			const double geographic = 0.65 * edge.midpoint.x - 0.35 * edge.midpoint.z;
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const double vertical_phase = 0.39 * static_cast<double>(k + 1);
				remap_state.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
					= 18.0 * std::sin(vertical_phase) + 7.5 * geographic;
			}
		}

		// Move a deterministic fraction of each even layer upward. The transferred
		// theta mass uses the source layer's theta, so each column remains exactly
		// conservative while becoming very far from the target mass coordinate.
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double geographic = 0.65 + 0.20 * std::abs(grid.cell(c).center.y);
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS - 1; k += 2) {
				const int lower = k * grid.cell_count() + c;
				const int upper = (k + 1) * grid.cell_count() + c;
				const double source_mass = remap_state.layer_mass_kg_m2[static_cast<size_t>(lower)];
				const double source_theta = remap_state.theta_mass_kg_k_m2[static_cast<size_t>(lower)]
					/ source_mass;
				const double moved_mass = source_mass * (0.10 + 0.08 * geographic);
				const double moved_theta_mass = moved_mass * source_theta;
				remap_state.layer_mass_kg_m2[static_cast<size_t>(lower)] -= moved_mass;
				remap_state.layer_mass_kg_m2[static_cast<size_t>(upper)] += moved_mass;
				remap_state.theta_mass_kg_k_m2[static_cast<size_t>(lower)] -= moved_theta_mass;
				remap_state.theta_mass_kg_k_m2[static_cast<size_t>(upper)] += moved_theta_mass;
			}
		}

		std::vector<double> remap_column_mass_before(static_cast<size_t>(grid.cell_count()), 0.0);
		std::vector<double> remap_column_theta_before(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				remap_column_mass_before[static_cast<size_t>(c)] += remap_state.layer_mass_kg_m2[static_cast<size_t>(i)];
				remap_column_theta_before[static_cast<size_t>(c)] += remap_state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			}
		}
		const double remap_global_mass_before = vertical.horizontal_transport().total_dry_mass_kg(remap_state);
		const double remap_global_theta_before = vertical.horizontal_transport().total_theta_mass_kg_k(remap_state);
		const auto remap_momentum_before = integrated_edge_momentum(grid, remap_state);

		const auto remap_diag = vertical.remap_to_reference_levels(remap_state);
		require(remap_diag.relative_dry_mass_error < 5e-13,
			"coordinate remap drifted in global dry mass");
		require(remap_diag.relative_theta_mass_error < 5e-13,
			"coordinate remap drifted in global theta mass");
		require(remap_diag.max_column_mass_error < 5e-13,
			"coordinate remap drifted in column dry mass");
		require(remap_diag.max_column_theta_mass_error < 5e-13,
			"coordinate remap drifted in column theta mass");
		require(remap_diag.max_edge_momentum_error < 5e-13,
			"coordinate remap drifted in vertically integrated edge momentum");
		require(remap_diag.max_mass_fraction_error < 5e-15,
			"coordinate remap did not restore reference level mass fractions");
		require(remap_diag.min_layer_mass_kg_m2 > 0.0,
			"coordinate remap produced non-positive layer mass");
		require(remap_diag.min_potential_temperature_k > 150.0,
			"coordinate remap produced invalid potential temperature");

		double remap_max_column_mass_error = 0.0;
		double remap_max_column_theta_error = 0.0;
		const auto &fractions = vertical.reference_mass_fractions();
		for (int c = 0; c < grid.cell_count(); ++c) {
			double column_mass_after = 0.0;
			double column_theta_after = 0.0;
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				column_mass_after += remap_state.layer_mass_kg_m2[static_cast<size_t>(i)];
				column_theta_after += remap_state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			}
			remap_max_column_mass_error = std::max(remap_max_column_mass_error,
				relative_error(column_mass_after, remap_column_mass_before[static_cast<size_t>(c)]));
			remap_max_column_theta_error = std::max(remap_max_column_theta_error,
				relative_error(column_theta_after, remap_column_theta_before[static_cast<size_t>(c)]));
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const double fraction = remap_state.layer_mass_kg_m2[
					static_cast<size_t>(k * grid.cell_count() + c)] / column_mass_after;
				require(std::abs(fraction - fractions[static_cast<size_t>(k)]) < 5e-15,
					"coordinate-remapped level has wrong mass fraction");
			}
		}
		require(relative_error(vertical.horizontal_transport().total_dry_mass_kg(remap_state),
			remap_global_mass_before) < 5e-13,
			"coordinate remap failed independent global mass check");
		require(relative_error(vertical.horizontal_transport().total_theta_mass_kg_k(remap_state),
			remap_global_theta_before) < 5e-13,
			"coordinate remap failed independent global theta check");
		require(remap_max_column_mass_error < 5e-13,
			"coordinate remap failed independent column mass check");
		require(remap_max_column_theta_error < 5e-13,
			"coordinate remap failed independent column theta check");

		const auto remap_momentum_after = integrated_edge_momentum(grid, remap_state);
		double independent_momentum_error = 0.0;
		for (int e = 0; e < grid.edge_count(); ++e) {
			independent_momentum_error = std::max(independent_momentum_error,
				relative_error(remap_momentum_after[static_cast<size_t>(e)],
					remap_momentum_before[static_cast<size_t>(e)]));
		}
		require(independent_momentum_error < 5e-13,
			"coordinate remap failed independent edge-momentum check");

		// Both vertically transported and coordinate-remapped states must define
		// valid hydrostatic columns everywhere.
		const auto hydro = vertical.horizontal_transport().diagnose_hydrostatic(state);
		const auto remap_hydro = vertical.horizontal_transport().diagnose_hydrostatic(remap_state);
		for (int c = 0; c < grid.cell_count(); ++c) {
			require(std::isfinite(hydro.surface_pressure_pa[static_cast<size_t>(c)])
				&& hydro.surface_pressure_pa[static_cast<size_t>(c)] > 7500.0,
				"vertical exchange produced invalid surface pressure");
			require(std::isfinite(remap_hydro.surface_pressure_pa[static_cast<size_t>(c)])
				&& remap_hydro.surface_pressure_pa[static_cast<size_t>(c)] > 7500.0,
				"coordinate remap produced invalid surface pressure");
		}

		std::cout << "VoronoiDryVerticalTransport PASS\n"
			<< "  accepted/requested dt: " << diag.accepted_dt_s << "/" << requested_dt << "\n"
			<< "  max vertical CFL: " << diag.max_vertical_courant << "\n"
			<< "  exchange max column mass/theta errors: " << max_column_mass_error
			<< "/" << max_column_theta_error << "\n"
			<< "  exchange global mass/theta errors: " << total_mass_error
			<< "/" << total_theta_error << "\n"
			<< "  remap global mass/theta errors: " << remap_diag.relative_dry_mass_error
			<< "/" << remap_diag.relative_theta_mass_error << "\n"
			<< "  remap column mass/theta errors: " << remap_diag.max_column_mass_error
			<< "/" << remap_diag.max_column_theta_mass_error << "\n"
			<< "  remap momentum/fraction errors: " << independent_momentum_error
			<< "/" << remap_diag.max_mass_fraction_error << "\n"
			<< "  min remap layer mass/theta: " << remap_diag.min_layer_mass_kg_m2
			<< "/" << remap_diag.min_potential_temperature_k << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryVerticalTransport FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
