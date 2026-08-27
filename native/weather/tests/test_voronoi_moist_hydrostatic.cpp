#include "voronoi_moist_hydrostatic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryTransport;
using asterra::weather::VoronoiMoistHydrostatic;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double G = 9.80665;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

bool nearly_equal(double a, double b, double relative = 5e-13, double absolute = 1e-11) {
	return std::abs(a - b) <= absolute
		+ relative * std::max({std::abs(a), std::abs(b), 1.0});
}

double max_abs_difference(const std::vector<double> &a,
		const std::vector<double> &b) {
	require(a.size() == b.size(), "diagnostic arrays have different shapes");
	double maximum = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		maximum = std::max(maximum, std::abs(a[i] - b[i]));
	}
	return maximum;
}

} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryTransport transport(grid, G, 8000.0, 7500.0);
		VoronoiMoistThermodynamics thermo(transport);
		VoronoiMoistHydrostatic moist_hydro(transport);

		// Dry-limit equivalence on a horizontally nonuniform state so the pressure-
		// gradient operator is tested as well as the column diagnostics.
		auto dry_limit = transport.make_isothermal_reference(PS, 288.0);
		thermo.ensure_water_tracers(dry_limit);
		for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
			for (int c = 0; c < grid.cell_count(); ++c) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const double factor = 1.0 + 0.01 * grid.cell(c).center.x;
				dry_limit.layer_mass_kg_m2[i] *= factor;
				dry_limit.theta_mass_kg_k_m2[i] *= factor;
			}
		}
		const auto dry_diag = transport.diagnose_hydrostatic(dry_limit);
		const auto moist_dry_diag = moist_hydro.diagnose(dry_limit);
		require(max_abs_difference(dry_diag.surface_pressure_pa,
			moist_dry_diag.surface_pressure_pa) < 1e-9,
			"moist hydrostatic dry limit changed surface pressure");
		require(max_abs_difference(dry_diag.interface_pressure_pa,
			moist_dry_diag.interface_pressure_pa) < 1e-9,
			"moist hydrostatic dry limit changed interface pressure");
		require(max_abs_difference(dry_diag.layer_pressure_pa,
			moist_dry_diag.layer_pressure_pa) < 1e-9,
			"moist hydrostatic dry limit changed layer pressure");
		require(max_abs_difference(dry_diag.temperature_k,
			moist_dry_diag.temperature_k) < 1e-10,
			"moist hydrostatic dry limit changed temperature");
		require(max_abs_difference(dry_diag.layer_geopotential,
			moist_dry_diag.layer_geopotential) < 1e-8,
			"moist hydrostatic dry limit changed layer geopotential");
		require(max_abs_difference(dry_diag.interface_geopotential,
			moist_dry_diag.interface_geopotential) < 1e-8,
			"moist hydrostatic dry limit changed interface geopotential");
		for (size_t i = 0; i < moist_dry_diag.temperature_k.size(); ++i) {
			require(moist_dry_diag.virtual_temperature_k[i]
				== moist_dry_diag.temperature_k[i],
				"zero-water virtual temperature is not exactly dry temperature");
		}
		const auto dry_accel = transport.pressure_gradient_acceleration(dry_limit, dry_diag);
		const auto moist_dry_accel = moist_hydro.pressure_gradient_acceleration(
			dry_limit, moist_dry_diag);
		require(max_abs_difference(dry_accel, moist_dry_accel) < 1e-12,
			"moist pressure-gradient operator does not recover dry limit");

		// Uniform vapor loading: total mechanical pressure must increase by exactly
		// g times the atmospheric water column. The prognostic dry-coordinate theta
		// still diagnoses the same sensible temperature; moisture changes Tv and
		// mechanical pressure rather than acting as an unaccounted heat source.
		auto vapor_state = transport.make_isothermal_reference(PS, 288.0);
		thermo.ensure_water_tracers(vapor_state);
		constexpr double QV = 0.012;
		for (size_t i = 0; i < vapor_state.layer_mass_kg_m2.size(); ++i) {
			vapor_state.tracer_mass_kg_m2[0][i] = QV * vapor_state.layer_mass_kg_m2[i];
		}
		const auto vapor_dry_diag = transport.diagnose_hydrostatic(vapor_state);
		const auto vapor_diag = moist_hydro.diagnose(vapor_state);
		for (int c = 0; c < grid.cell_count(); ++c) {
			double water_column = 0.0;
			for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				water_column += vapor_state.tracer_mass_kg_m2[0][i];
			}
			const double expected_ps = vapor_dry_diag.surface_pressure_pa[static_cast<size_t>(c)]
				+ G * water_column;
			require(nearly_equal(vapor_diag.surface_pressure_pa[static_cast<size_t>(c)],
				expected_ps, 2e-13, 1e-9),
				"moist surface pressure does not include exact vapor column weight");
		}
		for (size_t i = 0; i < vapor_diag.temperature_k.size(); ++i) {
			require(vapor_diag.temperature_k[i] == vapor_dry_diag.temperature_k[i],
				"water loading reinterpreted dry-coordinate theta as sensible heating");
			require(vapor_diag.virtual_temperature_k[i] > vapor_diag.temperature_k[i],
				"vapor loading failed to increase virtual temperature");
			require(nearly_equal(vapor_diag.layer_total_mass_kg_m2[i],
				vapor_state.layer_mass_kg_m2[i]
					+ vapor_state.tracer_mass_kg_m2[0][i]),
				"moist layer total mass diagnostic is inconsistent");
		}
		const auto uniform_moist_accel = moist_hydro.pressure_gradient_acceleration(
			vapor_state, vapor_diag);
		double max_uniform_accel = 0.0;
		for (double a : uniform_moist_accel) max_uniform_accel = std::max(max_uniform_accel, std::abs(a));
		require(max_uniform_accel < 1e-13,
			"horizontally uniform moist atmosphere generated pressure-gradient acceleration");

		const double dry_mass = transport.total_dry_mass_kg(vapor_state);
		const double vapor_mass = transport.total_tracer_mass_kg(vapor_state, 0);
		require(nearly_equal(moist_hydro.total_moist_air_mass_kg(vapor_state),
			dry_mass + vapor_mass, 2e-13, 1.0),
			"total moist-air mass diagnostic does not close");

		// Suspended condensate contributes weight and density but not gas pressure
		// through its own equation of state. The implemented Tv identity must match
		// the thermodynamics helper cell-for-cell.
		auto loaded = vapor_state;
		constexpr double QL = 0.003;
		constexpr double QI = 0.001;
		for (size_t i = 0; i < loaded.layer_mass_kg_m2.size(); ++i) {
			loaded.tracer_mass_kg_m2[1][i] = QL * loaded.layer_mass_kg_m2[i];
			loaded.tracer_mass_kg_m2[2][i] = QI * loaded.layer_mass_kg_m2[i];
		}
		const auto loaded_diag = moist_hydro.diagnose(loaded);
		for (size_t i = 0; i < loaded_diag.temperature_k.size(); ++i) {
			const double expected_tv = VoronoiMoistThermodynamics::virtual_temperature_k(
				loaded_diag.temperature_k[i], QV, QL, QI);
			require(nearly_equal(loaded_diag.virtual_temperature_k[i], expected_tv),
				"moist hydrostatic virtual-temperature identity is inconsistent");
			require(loaded_diag.layer_total_mass_kg_m2[i]
				> vapor_diag.layer_total_mass_kg_m2[i],
				"condensate loading did not increase total layer mass");
		}

		// A horizontal moisture loading gradient should now be visible to the
		// diagnostic pressure force, demonstrating the feedback pathway.
		auto gradient = transport.make_isothermal_reference(PS, 288.0);
		thermo.ensure_water_tracers(gradient);
		for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
			for (int c = 0; c < grid.cell_count(); ++c) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const double q = 0.008 + 0.004 * grid.cell(c).center.x;
				gradient.tracer_mass_kg_m2[0][i] = q * gradient.layer_mass_kg_m2[i];
			}
		}
		const auto gradient_diag = moist_hydro.diagnose(gradient);
		const auto gradient_accel = moist_hydro.pressure_gradient_acceleration(
			gradient, gradient_diag);
		double max_gradient_accel = 0.0;
		for (double a : gradient_accel) max_gradient_accel = std::max(max_gradient_accel, std::abs(a));
		require(max_gradient_accel > 1e-8,
			"horizontal moisture loading gradient produced no diagnostic pressure force");

		std::cout << "VoronoiMoistHydrostatic PASS\n"
			<< "  dry-limit max dPhi: "
			<< max_abs_difference(dry_diag.layer_geopotential,
				moist_dry_diag.layer_geopotential) << " m2/s2\n"
			<< "  uniform moist max accel: " << max_uniform_accel << " m/s2\n"
			<< "  moisture-gradient max accel: " << max_gradient_accel << " m/s2\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistHydrostatic FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
