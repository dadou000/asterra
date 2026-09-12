#include "voronoi_moist_thermodynamics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryTransport;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double total_species_kg(const VoronoiDryTransport::State &state,
		const GeodesicVoronoiGrid &grid, int tracer) {
	long double total = 0.0L;
	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		for (int c = 0; c < grid.cell_count(); ++c) {
			const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
			total += static_cast<long double>(state.tracer_mass_kg_m2[static_cast<size_t>(tracer)][i])
				* static_cast<long double>(grid.cell(c).area_m2);
		}
	}
	return static_cast<double>(total);
}

double max_relative_humidity(const VoronoiMoistThermodynamics &moist,
		const VoronoiDryTransport::State &state, int vapor_index) {
	const auto thermo = moist.diagnose_thermodynamics(state);
	double maximum = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double qv = state.tracer_mass_kg_m2[static_cast<size_t>(vapor_index)][i]
			/ state.layer_mass_kg_m2[i];
		const double qs = VoronoiMoistThermodynamics::saturation_mixing_ratio(
			thermo.layer_pressure_pa[i], thermo.temperature_k[i]);
		maximum = std::max(maximum, qv / qs);
	}
	return maximum;
}

void require_dry_geometry_unchanged(const VoronoiDryTransport::State &before,
		const VoronoiDryTransport::State &after) {
	require(before.layer_mass_kg_m2 == after.layer_mass_kg_m2,
		"moist phase adjustment changed dry layer mass");
	require(before.edge_normal_mps == after.edge_normal_mps,
		"moist phase adjustment changed dry wind");
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryTransport transport(grid, 9.80665, 8000.0, 7500.0);
		VoronoiMoistThermodynamics moist(transport);

		const double es273 = VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(273.15);
		const double es300 = VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(300.0);
		require(es273 > 605.0 && es273 < 618.0,
			"moist saturation pressure is unreasonable at 273.15 K");
		require(es300 > 3500.0 && es300 < 3600.0,
			"moist saturation pressure is unreasonable at 300 K");

		// Uniform-RH initialization must solve its own added vapor weight rather
		// than evaluating qsat once on the dry pressure field.
		auto rh_check = transport.make_isothermal_reference(PS, 288.0);
		moist.initialize_uniform_relative_humidity(rh_check, 0.70);
		const auto rh_thermo = moist.diagnose_thermodynamics(rh_check);
		double max_rh_init_error = 0.0;
		for (size_t i = 0; i < rh_check.layer_mass_kg_m2.size(); ++i) {
			const double qv = rh_check.tracer_mass_kg_m2[0][i] / rh_check.layer_mass_kg_m2[i];
			const double qs = VoronoiMoistThermodynamics::saturation_mixing_ratio(
				rh_thermo.layer_pressure_pa[i], rh_thermo.temperature_k[i]);
			max_rh_init_error = std::max(max_rh_init_error, std::abs(qv / qs - 0.70));
		}
		require(max_rh_init_error < 3e-10,
			"uniform-RH initialization is not self-consistent with full pressure");

		// Warm supersaturation: vapor must condense to liquid and warm the air.
		auto warm = transport.make_isothermal_reference(PS, 288.0);
		moist.initialize_uniform_relative_humidity(warm, 1.15);
		const auto warm_initial_thermo = moist.diagnose_thermodynamics(warm);
		const auto warm_before = warm;
		const auto warm_diag = moist.saturation_adjust(warm);
		require_dry_geometry_unchanged(warm_before, warm);
		require(warm_diag.condensed_water_kg > 0.0,
			"warm supersaturated atmosphere produced no condensation");
		require(warm_diag.evaporated_water_kg == 0.0,
			"warm supersaturation unexpectedly evaporated condensate");
		require(warm_diag.relative_total_water_error < 5e-12,
			"warm saturation adjustment changed total water");
		require(warm_diag.max_specific_enthalpy_error_j_kg < 1e-4,
			"warm saturation adjustment violated enthalpy conservation");
		require(warm_diag.max_relative_humidity_after <= 1.00000001,
			"warm saturation adjustment remained supersaturated");
		require(warm_diag.max_abs_temperature_change_k > 0.05,
			"warm condensation produced no latent heating");
		const auto warm_thermo = moist.diagnose_thermodynamics(warm);
		for (size_t i = 0; i < warm_thermo.temperature_k.size(); ++i) {
			require(warm_thermo.temperature_k[i] > warm_initial_thermo.temperature_k[i],
				"warm condensation failed to increase full-pressure temperature");
		}
		require(total_species_kg(warm, grid, 1) > 0.0,
			"warm condensation produced no cloud liquid");
		require(total_species_kg(warm, grid, 2) < 1e-9 * total_species_kg(warm, grid, 1),
			"warm cloud incorrectly produced significant ice");

		// Cold supersaturation: at 248 K condensate should remain entirely ice.
		auto cold = transport.make_isothermal_reference(PS, 248.0);
		moist.initialize_uniform_relative_humidity(cold, 1.05);
		const auto cold_before = cold;
		const auto cold_diag = moist.saturation_adjust(cold);
		require_dry_geometry_unchanged(cold_before, cold);
		require(cold_diag.condensed_water_kg > 0.0,
			"cold supersaturated atmosphere produced no deposition");
		require(cold_diag.max_temperature_k < VoronoiMoistThermodynamics::T_ALL_ICE_K,
			"cold deposition warmed outside the all-ice regime");
		require(total_species_kg(cold, grid, 2) > 0.0,
			"cold deposition produced no cloud ice");
		require(total_species_kg(cold, grid, 1) == 0.0,
			"all-ice cloud produced liquid water");
		require(cold_diag.max_relative_humidity_after <= 1.00000001,
			"cold saturation adjustment remained supersaturated");

		// Mixed-phase supersaturation should partition condensate into both species.
		auto mixed = transport.make_isothermal_reference(PS, 263.0);
		moist.initialize_uniform_relative_humidity(mixed, 1.06);
		const auto mixed_diag = moist.saturation_adjust(mixed);
		require(mixed_diag.condensed_water_kg > 0.0,
			"mixed-phase supersaturation produced no condensate");
		require(total_species_kg(mixed, grid, 1) > 0.0
				&& total_species_kg(mixed, grid, 2) > 0.0,
			"mixed-phase cloud did not produce both liquid and ice");
		require(mixed_diag.max_relative_humidity_after <= 1.00000001,
			"mixed-phase adjustment remained supersaturated");

		// Subsaturated air with cloud liquid must evaporate and cool.
		auto evap = transport.make_isothermal_reference(PS, 288.0);
		moist.initialize_uniform_relative_humidity(evap, 0.25);
		for (size_t i = 0; i < evap.layer_mass_kg_m2.size(); ++i) {
			evap.tracer_mass_kg_m2[1][i] = 5.0e-4 * evap.layer_mass_kg_m2[i];
		}
		const auto evap_initial_thermo = moist.diagnose_thermodynamics(evap);
		const auto evap_before = evap;
		const auto evap_diag = moist.saturation_adjust(evap);
		require_dry_geometry_unchanged(evap_before, evap);
		require(evap_diag.evaporated_water_kg > 0.0,
			"subsaturated cloud produced no evaporation");
		require(evap_diag.condensed_water_kg == 0.0,
			"subsaturated cloud unexpectedly condensed more water");
		require(total_species_kg(evap, grid, 1) < 1e-10 * evap_diag.evaporated_water_kg,
			"subsaturated test did not fully evaporate cloud liquid");
		require(total_species_kg(evap, grid, 2) == 0.0,
			"warm evaporation created cloud ice");
		const auto evap_thermo = moist.diagnose_thermodynamics(evap);
		for (size_t i = 0; i < evap_thermo.temperature_k.size(); ++i) {
			require(evap_thermo.temperature_k[i] < evap_initial_thermo.temperature_k[i],
				"cloud evaporation failed to cool full-pressure temperature");
		}
		require(max_relative_humidity(moist, evap, 0) < 1.0,
			"fully evaporated subsaturated test became saturated");

		// Zero-water atmosphere is an exact no-op for thermodynamics.
		auto dry = transport.make_isothermal_reference(PS, 280.0);
		moist.ensure_water_tracers(dry);
		const auto dry_before = dry;
		const auto dry_diag = moist.saturation_adjust(dry);
		require(dry.layer_mass_kg_m2 == dry_before.layer_mass_kg_m2,
			"zero-water adjustment changed dry mass");
		require(dry.theta_mass_kg_k_m2 == dry_before.theta_mass_kg_k_m2,
			"zero-water adjustment changed theta");
		require(dry.edge_normal_mps == dry_before.edge_normal_mps,
			"zero-water adjustment changed wind");
		require(dry_diag.total_water_after_kg == 0.0,
			"zero-water adjustment created water");

		std::cout << "VoronoiMoistThermodynamics PASS\n"
			<< "  full-pressure RH init max error: " << max_rh_init_error << "\n"
			<< "  warm condensed water: " << warm_diag.condensed_water_kg << " kg\n"
			<< "  warm max dT: " << warm_diag.max_abs_temperature_change_k << " K\n"
			<< "  cold condensed water: " << cold_diag.condensed_water_kg << " kg\n"
			<< "  mixed liquid/ice: " << total_species_kg(mixed, grid, 1)
			<< "/" << total_species_kg(mixed, grid, 2) << " kg\n"
			<< "  evaporated water: " << evap_diag.evaporated_water_kg << " kg\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistThermodynamics FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
