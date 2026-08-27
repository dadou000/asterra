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

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max({std::abs(a), std::abs(b), 1.0});
}

double species_mass(const VoronoiDryTransport &transport,
		const VoronoiDryTransport::State &state, int tracer) {
	return transport.total_tracer_mass_kg(state, tracer);
}

} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryTransport transport(grid, 9.80665, 8000.0, 7500.0);
		VoronoiMoistThermodynamics moist(transport);
		VoronoiSurfaceExchange exchange(transport);

		// Near-freezing mixed-phase atmosphere, initially just subsaturated. A
		// spatially sparse positive surface vapor pulse will cross saturation while
		// other cells experience dew. This exercises both directions of the surface
		// water transfer and subsequent liquid/ice phase partition in one cycle.
		auto atmosphere = transport.make_isothermal_reference(PS, 263.0);
		moist.initialize_uniform_relative_humidity(atmosphere, 0.98);
		auto surface = exchange.make_uniform_surface_state(20.0, 8.0e7);

		const auto dry_mass_before = atmosphere.layer_mass_kg_m2;
		const auto wind_before = atmosphere.edge_normal_mps;
		const auto surface_before = surface;
		const double system_water_before = exchange.total_atmospheric_water_kg(atmosphere)
			+ exchange.total_surface_water_kg(surface);
		const double system_energy_before = exchange.atmospheric_thermodynamic_energy_j(atmosphere)
			+ exchange.total_surface_energy_j(surface);

		std::vector<double> evaporation(static_cast<size_t>(grid.cell_count()), 0.0);
		std::vector<double> sensible(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			if (c % 5 == 0) {
				evaporation[static_cast<size_t>(c)] = 4.0e-5;
				sensible[static_cast<size_t>(c)] = 8.0;
			} else if (c % 5 == 1) {
				evaporation[static_cast<size_t>(c)] = -1.0e-5;
				sensible[static_cast<size_t>(c)] = -4.0;
			}
		}

		const auto exchange_diag = exchange.apply_fluxes(
			atmosphere, surface, evaporation, sensible, DT);
		require(exchange_diag.evaporated_to_atmosphere_kg > 0.0,
			"combined surface/moist cycle exercised no evaporation");
		require(exchange_diag.condensed_to_surface_kg > 0.0,
			"combined surface/moist cycle exercised no dew");
		require(exchange_diag.relative_system_water_error < 5e-12,
			"surface exchange water budget failed before phase adjustment");
		require(exchange_diag.relative_system_energy_error < 5e-12,
			"surface exchange energy budget failed before phase adjustment");

		const auto surface_after_exchange = surface;
		const double atmospheric_energy_before_phase =
			exchange.atmospheric_thermodynamic_energy_j(atmosphere);
		const double atmospheric_water_before_phase =
			exchange.total_atmospheric_water_kg(atmosphere);
		const double liquid_before_phase = species_mass(transport, atmosphere, 1);
		const double ice_before_phase = species_mass(transport, atmosphere, 2);

		const auto phase_diag = moist.saturation_adjust(atmosphere);
		require(phase_diag.condensed_water_kg > 0.0,
			"surface vapor pulse did not trigger cloud condensation/deposition");
		require(phase_diag.max_relative_humidity_after <= 1.00000001,
			"combined surface/moist cycle remained supersaturated");
		require(phase_diag.relative_total_water_error < 5e-12,
			"phase adjustment changed atmospheric total water");
		require(phase_diag.max_specific_enthalpy_error_j_kg < 1e-4,
			"phase adjustment violated local moist enthalpy conservation");

		// The phase operator owns only atmosphere-internal repartitioning. It must
		// not touch the surface reservoir, dry mass or momentum.
		require(surface.water_kg_m2 == surface_after_exchange.water_kg_m2
				&& surface.energy_j_m2 == surface_after_exchange.energy_j_m2,
			"phase adjustment changed surface reservoirs");
		require(atmosphere.layer_mass_kg_m2 == dry_mass_before,
			"combined source cycle changed dry atmospheric mass");
		require(atmosphere.edge_normal_mps == wind_before,
			"combined source cycle changed atmospheric momentum directly");

		const double atmospheric_water_after_phase =
			exchange.total_atmospheric_water_kg(atmosphere);
		const double atmospheric_energy_after_phase =
			exchange.atmospheric_thermodynamic_energy_j(atmosphere);
		require(relative_error(atmospheric_water_before_phase,
			atmospheric_water_after_phase) < 5e-12,
			"phase cycle changed atmospheric water after surface transfer");
		require(relative_error(atmospheric_energy_before_phase,
			atmospheric_energy_after_phase) < 2e-12,
			"phase cycle changed atmospheric thermodynamic energy after surface transfer");

		const double liquid_after_phase = species_mass(transport, atmosphere, 1);
		const double ice_after_phase = species_mass(transport, atmosphere, 2);
		require(liquid_after_phase > liquid_before_phase,
			"mixed-phase source cycle produced no additional cloud liquid");
		require(ice_after_phase > ice_before_phase,
			"mixed-phase source cycle produced no additional cloud ice");

		const double system_water_after = exchange.total_atmospheric_water_kg(atmosphere)
			+ exchange.total_surface_water_kg(surface);
		const double system_energy_after = exchange.atmospheric_thermodynamic_energy_j(atmosphere)
			+ exchange.total_surface_energy_j(surface);
		require(relative_error(system_water_before, system_water_after) < 5e-12,
			"combined surface+phase cycle failed closed total-water budget");
		require(relative_error(system_energy_before, system_energy_after) < 2e-12,
			"combined surface+phase cycle failed closed thermodynamic-energy budget");

		std::cout << "VoronoiSurfaceMoistCycle PASS\n"
			<< "  surface water error: " << exchange_diag.relative_system_water_error << "\n"
			<< "  surface energy error: " << exchange_diag.relative_system_energy_error << "\n"
			<< "  phase condensed: " << phase_diag.condensed_water_kg << " kg\n"
			<< "  liquid/ice gain: " << (liquid_after_phase - liquid_before_phase)
			<< "/" << (ice_after_phase - ice_before_phase) << " kg\n"
			<< "  closed water/energy error: "
			<< relative_error(system_water_before, system_water_after) << "/"
			<< relative_error(system_energy_before, system_energy_after) << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiSurfaceMoistCycle FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
