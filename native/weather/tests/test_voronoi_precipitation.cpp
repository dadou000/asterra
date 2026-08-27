#include "voronoi_precipitation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiMoistHydrostatic;
using asterra::weather::VoronoiMoistThermodynamics;
using asterra::weather::VoronoiPrecipitation;
using asterra::weather::VoronoiSurfaceExchange;

namespace {
constexpr double R = 3500000.0;
constexpr double G = 9.80665;
constexpr double PS = 110000.0;
constexpr double T = 278.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}

bool exact_nonprecip_state(const VoronoiPrecipitation::State &a,
		const VoronoiPrecipitation::State &b) {
	if (a.layer_mass_kg_m2 != b.layer_mass_kg_m2
			|| a.theta_mass_kg_k_m2 != b.theta_mass_kg_k_m2
			|| a.edge_normal_mps != b.edge_normal_mps) return false;
	for (int tracer = 0; tracer < 3; ++tracer) {
		if (a.tracer_mass_kg_m2[static_cast<size_t>(tracer)]
				!= b.tracer_mass_kg_m2[static_cast<size_t>(tracer)]) return false;
	}
	return true;
}

} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		asterra::weather::VoronoiDryTransport transport(grid, G, 8000.0, 7500.0);
		VoronoiMoistThermodynamics moist(transport);
		VoronoiPrecipitation precipitation(transport);
		VoronoiSurfaceExchange surface_exchange(transport);

		// No hydrometeors must be an exact no-op and must not restrict dt.
		auto dry = transport.make_isothermal_reference(PS, T);
		moist.initialize_uniform_relative_humidity(dry, 0.55);
		auto dry_surface = surface_exchange.make_uniform_surface_state(8.0, 1.0e7);
		const auto dry_before = dry;
		const auto dry_surface_before = dry_surface;
		const auto zero = precipitation.step(dry, dry_surface, 600.0);
		require(zero.accepted_dt_s == 600.0,
			"zero-precipitation column unnecessarily reduced timestep");
		require(dry.layer_mass_kg_m2 == dry_before.layer_mass_kg_m2
				&& dry.theta_mass_kg_k_m2 == dry_before.theta_mass_kg_k_m2
				&& dry.edge_normal_mps == dry_before.edge_normal_mps
				&& dry.tracer_mass_kg_m2 == dry_before.tracer_mass_kg_m2,
			"zero precipitation changed atmospheric state");
		require(dry_surface.water_kg_m2 == dry_surface_before.water_kg_m2
				&& dry_surface.energy_j_m2 == dry_surface_before.energy_j_m2,
			"zero precipitation changed surface reservoir");

		auto state = transport.make_isothermal_reference(PS, T);
		moist.initialize_uniform_relative_humidity(state, 0.55);
		precipitation.ensure_precipitation_tracers(state);
		auto surface = surface_exchange.make_uniform_surface_state(8.0, 1.0e7);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &p = grid.cell(c).center;
			const double horizontal = 0.6 + 0.4 * (0.5 + 0.5 * p.x);
			for (int k = 0; k < VoronoiPrecipitation::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const double dry_mass = state.layer_mass_kg_m2[i];
				if (k >= 8 && k <= 14) {
					state.tracer_mass_kg_m2[3][i] = dry_mass * 8.0e-4 * horizontal;
				}
				if (k >= 3 && k <= 8) {
					state.tracer_mass_kg_m2[4][i] = dry_mass * 5.0e-4 * (1.1 - 0.2 * p.y);
				}
			}
		}

		// The sedimentation CFL must account for every layer a hydrometeor can
		// enter during SSPRK, not only layers that are wet at the start of a stage.
		constexpr double RAIN_SPEED = 7.0;
		constexpr double SNOW_SPEED = 1.0;
		constexpr double TARGET_CFL = 0.45;
		const double stable = precipitation.stable_dt(
			state, TARGET_CFL, 600.0, RAIN_SPEED, SNOW_SPEED);
		VoronoiMoistHydrostatic hydrostatic(transport);
		const auto hydro = hydrostatic.diagnose(state);
		double min_dz = 1e300;
		for (int c = 0; c < grid.cell_count(); ++c) {
			for (int k = 0; k < VoronoiPrecipitation::LEVELS; ++k) {
				const double dz = (hydro.interface_geopotential[
					static_cast<size_t>((k + 1) * grid.cell_count() + c)]
					- hydro.interface_geopotential[
						static_cast<size_t>(k * grid.cell_count() + c)]) / G;
				min_dz = std::min(min_dz, dz);
			}
		}
		require(stable * RAIN_SPEED / min_dz <= TARGET_CFL + 1e-12,
			"precipitation stable_dt ignored a dry layer that sedimentation can enter");

		const auto nonprecip_reference = state;
		const auto surface_energy_reference = surface.energy_j_m2;
		const double atmosphere_water0 = precipitation.total_atmospheric_water_kg(state);
		const double surface_water0 = surface_exchange.total_surface_water_kg(surface);
		const double rain0 = precipitation.total_rain_kg(state);
		const double snow0 = precipitation.total_snow_kg(state);
		const double system_water0 = atmosphere_water0 + surface_water0;

		double elapsed = 0.0;
		constexpr double DURATION = 1800.0;
		int steps = 0;
		double cumulative_rain_deposit = 0.0;
		double cumulative_snow_deposit = 0.0;
		double max_water_error = 0.0;
		double max_surface_flux = 0.0;
		while (elapsed < DURATION) {
			const double request = std::min(300.0, DURATION - elapsed);
			const auto d = precipitation.step(state, surface, request,
				RAIN_SPEED, SNOW_SPEED, TARGET_CFL, 10);
			require(d.accepted_dt_s > 0.0,
				"precipitation sedimentation rejected all timesteps");
			require(d.max_courant <= TARGET_CFL + 1e-10,
				"precipitation exceeded configured CFL target");
			require(d.relative_system_water_error < 2.1e-11,
				"precipitation step did not close atmosphere+surface water");
			require(d.min_rain_kg_m2 >= 0.0 && d.min_snow_kg_m2 >= 0.0,
				"precipitation created negative hydrometeor mass");
		cumulative_rain_deposit += d.deposited_rain_kg;
		cumulative_snow_deposit += d.deposited_snow_kg;
		max_water_error = std::max(max_water_error, d.relative_system_water_error);
		max_surface_flux = std::max(max_surface_flux, d.max_surface_precip_flux_kg_m2_s);
		elapsed += d.accepted_dt_s;
		++steps;
		require(steps < 5000, "precipitation sedimentation used excessive substeps");
		}

		const double atmosphere_water1 = precipitation.total_atmospheric_water_kg(state);
		const double surface_water1 = surface_exchange.total_surface_water_kg(surface);
		const double rain1 = precipitation.total_rain_kg(state);
		const double snow1 = precipitation.total_snow_kg(state);
		require(relative_error(atmosphere_water1 + surface_water1, system_water0) < 5e-10,
			"multi-step precipitation drifted total atmosphere+surface water");
		require(rain1 < rain0 && snow1 < snow0,
			"rain/snow sedimentation produced no surface fallout");
		require(cumulative_rain_deposit > 0.0 && cumulative_snow_deposit > 0.0,
			"precipitation diagnostics reported no rain or snow deposition");
		require(relative_error(cumulative_rain_deposit, rain0 - rain1) < 5e-10,
			"cumulative rain deposition does not equal atmospheric rain loss");
		require(relative_error(cumulative_snow_deposit, snow0 - snow1) < 5e-10,
			"cumulative snow deposition does not equal atmospheric snow loss");
		require(max_surface_flux > 0.0,
			"precipitation produced no diagnostic surface mass flux");
		require(exact_nonprecip_state(state, nonprecip_reference),
			"sedimentation changed dry air, cloud/vapor tracers, theta or wind");
		require(surface.energy_j_m2 == surface_energy_reference,
			"water-only sedimentation changed surface energy reservoir");

		std::cout << "VoronoiPrecipitation PASS\n"
			<< "  stable dt: " << stable << " s\n"
			<< "  30min steps: " << steps << "\n"
			<< "  rain/snow deposited: " << cumulative_rain_deposit
			<< "/" << cumulative_snow_deposit << " kg\n"
			<< "  max system-water error: " << max_water_error << "\n"
			<< "  max surface flux: " << max_surface_flux << " kg/m2/s\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiPrecipitation FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
