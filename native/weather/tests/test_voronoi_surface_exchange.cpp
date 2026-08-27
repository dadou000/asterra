#include "voronoi_surface_exchange.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryTransport;
using asterra::weather::VoronoiMoistThermodynamics;
using asterra::weather::VoronoiSurfaceExchange;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double DT = 600.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

bool nearly_equal(double a, double b, double relative = 2e-12, double absolute = 1e-12) {
	return std::abs(a - b) <= absolute
		+ relative * std::max({std::abs(a), std::abs(b), 1.0});
}

bool state_exact_equal(const VoronoiDryTransport::State &a,
		const VoronoiDryTransport::State &b) {
	return a.layer_mass_kg_m2 == b.layer_mass_kg_m2
		&& a.theta_mass_kg_k_m2 == b.theta_mass_kg_k_m2
		&& a.edge_normal_mps == b.edge_normal_mps
		&& a.tracer_mass_kg_m2 == b.tracer_mass_kg_m2;
}

bool surface_exact_equal(const VoronoiSurfaceExchange::SurfaceState &a,
		const VoronoiSurfaceExchange::SurfaceState &b) {
	return a.water_kg_m2 == b.water_kg_m2
		&& a.energy_j_m2 == b.energy_j_m2;
}

long double integrate_cell_field(const GeodesicVoronoiGrid &grid,
		const std::vector<double> &field) {
	long double total = 0.0L;
	for (int c = 0; c < grid.cell_count(); ++c) {
		total += static_cast<long double>(field[static_cast<size_t>(c)])
			* static_cast<long double>(grid.cell(c).area_m2);
	}
	return total;
}

void require_transaction_rollback(VoronoiSurfaceExchange &exchange,
		VoronoiDryTransport::State atmosphere,
		VoronoiSurfaceExchange::SurfaceState surface,
		const std::vector<double> &evaporation,
		const std::vector<double> &sensible,
		double dt, const char *message) {
	const auto atmosphere_before = atmosphere;
	const auto surface_before = surface;
	bool rejected = false;
	try {
		(void)exchange.apply_fluxes(atmosphere, surface, evaporation, sensible, dt);
	} catch (const std::exception &) {
		rejected = true;
	}
	require(rejected, message);
	require(state_exact_equal(atmosphere, atmosphere_before),
		"rejected surface exchange did not restore atmospheric state exactly");
	require(surface_exact_equal(surface, surface_before),
		"rejected surface exchange did not restore surface state exactly");
}

} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryTransport transport(grid, 9.80665, 8000.0, 7500.0);
		VoronoiMoistThermodynamics moist(transport);
		VoronoiSurfaceExchange exchange(transport);

		auto atmosphere = transport.make_isothermal_reference(PS, 288.0);
		moist.initialize_uniform_relative_humidity(atmosphere, 0.45);
		auto surface = exchange.make_uniform_surface_state(30.0, 5.0e7);
		const auto atmosphere_before = atmosphere;
		const auto surface_before = surface;
		const auto thermo_before = moist.diagnose_thermodynamics(atmosphere);

		const int cells = grid.cell_count();
		std::vector<double> evaporation(static_cast<size_t>(cells), 0.0);
		std::vector<double> sensible(static_cast<size_t>(cells), 0.0);
		for (int c = 0; c < cells; ++c) {
			if (c % 3 == 0) evaporation[static_cast<size_t>(c)] = 2.0e-5;
			else if (c % 3 == 1) evaporation[static_cast<size_t>(c)] = -1.0e-5;
			sensible[static_cast<size_t>(c)] = (c % 2 == 0) ? 40.0 : -25.0;
		}

		const long double expected_water_to_atmosphere =
			integrate_cell_field(grid, evaporation) * static_cast<long double>(DT);
		const long double expected_sensible_to_atmosphere =
			integrate_cell_field(grid, sensible) * static_cast<long double>(DT);
		const long double expected_latent_to_atmosphere =
			expected_water_to_atmosphere
			* static_cast<long double>(VoronoiMoistThermodynamics::LV0_J_KG);

		const auto diagnostics = exchange.apply_fluxes(
			atmosphere, surface, evaporation, sensible, DT);

		require(diagnostics.relative_system_water_error < 5e-13,
			"surface exchange failed atmosphere+surface water conservation");
		require(diagnostics.relative_system_energy_error < 5e-13,
			"surface exchange failed atmosphere+surface energy conservation");
		require(diagnostics.evaporated_to_atmosphere_kg > 0.0,
			"surface exchange exercised no evaporation");
		require(diagnostics.condensed_to_surface_kg > 0.0,
			"surface exchange exercised no dew/condensation");
		require(nearly_equal(diagnostics.sensible_to_atmosphere_j,
			static_cast<double>(expected_sensible_to_atmosphere), 2e-12, 1.0),
			"surface sensible-energy diagnostic disagrees with prescribed flux integral");
		require(nearly_equal(diagnostics.latent_to_atmosphere_j,
			static_cast<double>(expected_latent_to_atmosphere), 2e-12, 1.0),
			"surface latent-energy diagnostic disagrees with prescribed flux integral");

		require(atmosphere.layer_mass_kg_m2 == atmosphere_before.layer_mass_kg_m2,
			"surface exchange changed dry atmospheric mass");
		require(atmosphere.edge_normal_mps == atmosphere_before.edge_normal_mps,
			"surface exchange changed wind directly");
		for (int k = 1; k < VoronoiDryTransport::LEVELS; ++k) {
			for (int c = 0; c < cells; ++c) {
				const size_t i = static_cast<size_t>(k * cells + c);
				require(atmosphere.theta_mass_kg_k_m2[i]
					== atmosphere_before.theta_mass_kg_k_m2[i],
					"surface source leaked theta above the bottom layer");
				for (size_t tracer = 0; tracer < atmosphere.tracer_mass_kg_m2.size(); ++tracer) {
					require(atmosphere.tracer_mass_kg_m2[tracer][i]
						== atmosphere_before.tracer_mass_kg_m2[tracer][i],
						"surface water exchange leaked above the bottom layer");
				}
			}
		}

		const auto thermo_after = moist.diagnose_thermodynamics(atmosphere);
		long double direct_atmosphere_water_change = 0.0L;
		long double direct_surface_water_change = 0.0L;
		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(c);
			const double dm = evaporation[static_cast<size_t>(c)] * DT;
			const double sensible_j_m2 = sensible[static_cast<size_t>(c)] * DT;
			const double vapor_delta = atmosphere.tracer_mass_kg_m2[0][i]
				- atmosphere_before.tracer_mass_kg_m2[0][i];
			const double surface_delta = surface.water_kg_m2[static_cast<size_t>(c)]
				- surface_before.water_kg_m2[static_cast<size_t>(c)];
			require(nearly_equal(vapor_delta, dm),
				"bottom-layer vapor transfer is not the prescribed water mass");
			require(nearly_equal(surface_delta, -dm),
				"surface reservoir transfer is not equal/opposite to vapor transfer");
			const long double area = static_cast<long double>(grid.cell(c).area_m2);
			direct_atmosphere_water_change += static_cast<long double>(vapor_delta) * area;
			direct_surface_water_change += static_cast<long double>(surface_delta) * area;
			const double expected_temperature = thermo_before.temperature_k[i]
				+ sensible_j_m2
					/ (VoronoiMoistThermodynamics::CP_DRY
						* atmosphere_before.layer_mass_kg_m2[i]);
			require(nearly_equal(thermo_after.temperature_k[i], expected_temperature, 5e-13, 1e-11),
				"full-pressure bottom temperature did not receive prescribed sensible energy");
		}

		require(nearly_equal(static_cast<double>(direct_atmosphere_water_change),
			static_cast<double>(expected_water_to_atmosphere), 3e-12, 1.0),
			"area-integrated atmospheric water change disagrees with prescribed flux");
		require(nearly_equal(static_cast<double>(direct_surface_water_change),
			-static_cast<double>(expected_water_to_atmosphere), 3e-12, 1.0),
			"area-integrated surface water change is not equal/opposite to atmosphere");
		require(nearly_equal(static_cast<double>(
			direct_atmosphere_water_change + direct_surface_water_change), 0.0,
			5e-12, 1.0),
			"direct atmosphere+surface water transfer does not close");

		// Pure evaporation changes full pressure. With zero sensible heat, the
		// physical temperature must nevertheless stay fixed, which requires theta
		// to shift. This catches accidental use of dry pressure in the source path.
		auto latent_only_atmosphere = atmosphere_before;
		auto latent_only_surface = surface_before;
		const auto latent_before_thermo = moist.diagnose_thermodynamics(latent_only_atmosphere);
		const auto latent_before_theta = latent_only_atmosphere.theta_mass_kg_k_m2;
		auto latent_evap = std::vector<double>(static_cast<size_t>(cells), 0.0);
		auto zero = std::vector<double>(static_cast<size_t>(cells), 0.0);
		latent_evap[0] = 2.0e-5;
		const auto latent_diag = exchange.apply_fluxes(
			latent_only_atmosphere, latent_only_surface, latent_evap, zero, DT);
		const auto latent_after_thermo = moist.diagnose_thermodynamics(latent_only_atmosphere);
		require(nearly_equal(latent_after_thermo.temperature_k[0],
			latent_before_thermo.temperature_k[0], 5e-13, 1e-11),
			"evaporation without sensible heat spuriously changed physical temperature");
		require(latent_only_atmosphere.theta_mass_kg_k_m2[0] != latent_before_theta[0],
			"evaporation changed full pressure but theta was not compensated");
		require(latent_diag.relative_system_energy_error < 5e-13,
			"latent-only surface exchange failed full-pressure energy closure");

		// A zero source step remains an exact no-op.
		auto zero_atmosphere = atmosphere_before;
		auto zero_surface = surface_before;
		const auto zero_atmosphere_before = zero_atmosphere;
		const auto zero_surface_before = zero_surface;
		const auto zero_diag = exchange.apply_fluxes(
			zero_atmosphere, zero_surface, zero, zero, DT);
		require(state_exact_equal(zero_atmosphere, zero_atmosphere_before),
			"zero-flux surface exchange changed atmospheric state");
		require(surface_exact_equal(zero_surface, zero_surface_before),
			"zero-flux surface exchange changed surface state");
		require(zero_diag.relative_system_water_error == 0.0,
			"zero-flux surface exchange changed total water");

		auto dry_surface = exchange.make_uniform_surface_state(0.0, 5.0e7);
		auto positive_evap = zero;
		positive_evap[0] = 1.0e-3;
		require_transaction_rollback(exchange, atmosphere_before, dry_surface,
			positive_evap, zero, DT,
			"surface exchange accepted evaporation without donor water");

		auto no_vapor = atmosphere_before;
		no_vapor.tracer_mass_kg_m2[0][0] = 0.0;
		auto negative_evap = zero;
		negative_evap[0] = -1.0e-3;
		require_transaction_rollback(exchange, no_vapor, surface_before,
			negative_evap, zero, DT,
			"surface exchange accepted condensation without donor vapor");

		auto extreme_cooling = zero;
		extreme_cooling[0] = -1.0e9;
		require_transaction_rollback(exchange, atmosphere_before, surface_before,
			zero, extreme_cooling, DT,
			"surface exchange accepted a non-physical bottom-layer temperature");

		std::cout << "VoronoiSurfaceExchange PASS\n"
			<< "  water error: " << diagnostics.relative_system_water_error << "\n"
			<< "  energy error: " << diagnostics.relative_system_energy_error << "\n"
			<< "  latent-only energy error: " << latent_diag.relative_system_energy_error << "\n"
			<< "  evaporation/dew: " << diagnostics.evaporated_to_atmosphere_kg
			<< "/" << diagnostics.condensed_to_surface_kg << " kg\n"
			<< "  bottom T range: " << diagnostics.min_bottom_temperature_k
			<< ".." << diagnostics.max_bottom_temperature_k << " K\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiSurfaceExchange FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
