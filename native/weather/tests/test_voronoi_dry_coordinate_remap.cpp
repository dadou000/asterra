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
constexpr double POSITIVE_BUDGET_TOL = 5e-13;
constexpr double SIGNED_MOMENTUM_TOL = 2e-11;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}

std::vector<double> integrated_edge_momentum(
		const GeodesicVoronoiGrid &grid,
		const VoronoiDryVerticalTransport::State &state) {
	std::vector<double> out(static_cast<size_t>(grid.edge_count()), 0.0);
	for (int e = 0; e < grid.edge_count(); ++e) {
		const auto &edge = grid.edge(e);
		long double sum = 0.0L;
		for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
			const int ia = k * grid.cell_count() + edge.cell_a;
			const int ib = k * grid.cell_count() + edge.cell_b;
			const int ie = k * grid.edge_count() + e;
			const double edge_mass = 0.5 * (
				state.layer_mass_kg_m2[static_cast<size_t>(ia)]
				+ state.layer_mass_kg_m2[static_cast<size_t>(ib)]);
			sum += static_cast<long double>(edge_mass)
				* static_cast<long double>(state.edge_normal_mps[static_cast<size_t>(ie)]);
		}
		out[static_cast<size_t>(e)] = static_cast<double>(sum);
	}
	return out;
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(5, R);
		VoronoiDryVerticalTransport vertical(grid);
		auto state = vertical.make_isothermal_reference(PS, T);

		// Strong vertical shear makes the momentum check sensitive to whether the
		// remapper moves extensive edge momentum or merely interpolates velocity.
		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			const double geographic = 0.65 * edge.midpoint.x - 0.35 * edge.midpoint.z;
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				state.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
					= 18.0 * std::sin(0.39 * static_cast<double>(k + 1))
					+ 7.5 * geographic;
			}
		}

		// Deliberately destroy the target layer-mass distribution while conserving
		// every column's mass and theta mass. Each even layer donates mass upward,
		// carrying its donor potential temperature with it.
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double geographic = 0.65 + 0.20 * std::abs(grid.cell(c).center.y);
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS - 1; k += 2) {
				const int lower = k * grid.cell_count() + c;
				const int upper = (k + 1) * grid.cell_count() + c;
				const double source_mass = state.layer_mass_kg_m2[static_cast<size_t>(lower)];
				const double theta = state.theta_mass_kg_k_m2[static_cast<size_t>(lower)] / source_mass;
				const double moved_mass = source_mass * (0.10 + 0.08 * geographic);
				const double moved_theta_mass = moved_mass * theta;
				state.layer_mass_kg_m2[static_cast<size_t>(lower)] -= moved_mass;
				state.layer_mass_kg_m2[static_cast<size_t>(upper)] += moved_mass;
				state.theta_mass_kg_k_m2[static_cast<size_t>(lower)] -= moved_theta_mass;
				state.theta_mass_kg_k_m2[static_cast<size_t>(upper)] += moved_theta_mass;
			}
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
		const double global_mass_before = vertical.horizontal_transport().total_dry_mass_kg(state);
		const double global_theta_before = vertical.horizontal_transport().total_theta_mass_kg_k(state);
		const auto momentum_before = integrated_edge_momentum(grid, state);

		const auto d = vertical.remap_to_reference_levels(state);
		require(d.relative_dry_mass_error < POSITIVE_BUDGET_TOL,
			"coordinate remap drifted in global dry mass");
		require(d.relative_theta_mass_error < POSITIVE_BUDGET_TOL,
			"coordinate remap drifted in global theta mass");
		require(d.max_column_mass_error < POSITIVE_BUDGET_TOL,
			"coordinate remap drifted in column dry mass");
		require(d.max_column_theta_mass_error < POSITIVE_BUDGET_TOL,
			"coordinate remap drifted in column theta mass");
		require(d.max_edge_momentum_error < SIGNED_MOMENTUM_TOL,
			"coordinate remap exceeded signed edge-momentum budget");
		require(d.max_mass_fraction_error < 5e-15,
			"coordinate remap did not restore reference mass fractions");
		require(d.min_layer_mass_kg_m2 > 0.0,
			"coordinate remap produced non-positive layer mass");
		require(d.min_potential_temperature_k > 150.0,
			"coordinate remap produced invalid theta");

		const auto &fraction = vertical.reference_mass_fractions();
		double max_fraction_error = 0.0;
		double max_column_mass_error = 0.0;
		double max_column_theta_error = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			double mass_after = 0.0;
			double theta_after = 0.0;
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const int i = k * grid.cell_count() + c;
				mass_after += state.layer_mass_kg_m2[static_cast<size_t>(i)];
				theta_after += state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			}
			max_column_mass_error = std::max(max_column_mass_error,
				relative_error(mass_after, column_mass_before[static_cast<size_t>(c)]));
			max_column_theta_error = std::max(max_column_theta_error,
				relative_error(theta_after, column_theta_before[static_cast<size_t>(c)]));
			for (int k = 0; k < VoronoiDryVerticalTransport::LEVELS; ++k) {
				const double actual = state.layer_mass_kg_m2[
					static_cast<size_t>(k * grid.cell_count() + c)] / mass_after;
				max_fraction_error = std::max(max_fraction_error,
					std::abs(actual - fraction[static_cast<size_t>(k)]));
			}
		}

		const auto momentum_after = integrated_edge_momentum(grid, state);
		double max_momentum_error = 0.0;
		for (int e = 0; e < grid.edge_count(); ++e) {
			max_momentum_error = std::max(max_momentum_error,
				relative_error(momentum_after[static_cast<size_t>(e)],
					momentum_before[static_cast<size_t>(e)]));
		}

		require(relative_error(vertical.horizontal_transport().total_dry_mass_kg(state),
			global_mass_before) < POSITIVE_BUDGET_TOL,
			"independent global mass check failed");
		require(relative_error(vertical.horizontal_transport().total_theta_mass_kg_k(state),
			global_theta_before) < POSITIVE_BUDGET_TOL,
			"independent global theta check failed");
		require(max_column_mass_error < POSITIVE_BUDGET_TOL,
			"independent column mass check failed");
		require(max_column_theta_error < POSITIVE_BUDGET_TOL,
			"independent column theta check failed");
		require(max_momentum_error < SIGNED_MOMENTUM_TOL,
			"independent edge momentum check failed");
		require(max_fraction_error < 5e-15,
			"independent reference fraction check failed");

		const auto hydro = vertical.horizontal_transport().diagnose_hydrostatic(state);
		for (double ps : hydro.surface_pressure_pa) {
			require(std::isfinite(ps) && ps > 7500.0,
				"coordinate remap produced invalid hydrostatic pressure");
		}

		std::cout << "VoronoiDryCoordinateRemap PASS\n"
			<< "  global mass/theta errors: " << d.relative_dry_mass_error
			<< "/" << d.relative_theta_mass_error << "\n"
			<< "  max column mass/theta errors: " << max_column_mass_error
			<< "/" << max_column_theta_error << "\n"
			<< "  max signed edge-momentum error: " << max_momentum_error << "\n"
			<< "  max mass-fraction error: " << max_fraction_error << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiDryCoordinateRemap FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
