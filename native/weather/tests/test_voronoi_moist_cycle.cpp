#include "voronoi_dry_core.h"
#include "voronoi_moist_thermodynamics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 288.0;
constexpr double OMEGA = 2.0 * PI / (11.5 * 3600.0);

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}

double total_water(const VoronoiDryCore &core, const VoronoiDryCore::State &state) {
	return core.total_tracer_mass_kg(state, 0)
		+ core.total_tracer_mass_kg(state, 1)
		+ core.total_tracer_mass_kg(state, 2);
}

double max_relative_humidity(const VoronoiDryCore &core,
		const VoronoiDryCore::State &state) {
	VoronoiMoistThermodynamics moist(core.transport());
	const auto thermo = moist.diagnose_thermodynamics(state);
	double maximum = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double qv = state.tracer_mass_kg_m2[0][i] / state.layer_mass_kg_m2[i];
		const double es = VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(
			thermo.temperature_k[i]);
		if (!(thermo.layer_pressure_pa[i] > es)) continue;
		const double qs = VoronoiMoistThermodynamics::EPSILON * es
			/ (thermo.layer_pressure_pa[i] - es);
		maximum = std::max(maximum, qv / qs);
	}
	return maximum;
}

void require_water_nonnegative(const VoronoiDryCore::State &state) {
	for (int tracer = 0; tracer < 3; ++tracer) {
		for (double mass : state.tracer_mass_kg_m2[static_cast<size_t>(tracer)]) {
			require(std::isfinite(mass) && mass >= 0.0,
				"coupled moist cycle produced negative/non-finite water mass");
		}
	}
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(6, R);
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});
		auto state = core.make_isothermal_reference(PS, T);
		VoronoiMoistThermodynamics moist(core.transport());
		moist.initialize_uniform_relative_humidity(state, 0.72);

		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			const double vertical = static_cast<double>(k)
				/ static_cast<double>(VoronoiDryCore::LEVELS - 1);
			for (int c = 0; c < grid.cell_count(); ++c) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const auto &p = grid.cell(c).center;
				const double dry = state.layer_mass_kg_m2[i];
				if (p.x > 0.15) {
					const double boost = 1.62 - 0.12 * vertical;
					state.tracer_mass_kg_m2[0][i] *= boost;
				}
				if (p.x < -0.20 && std::abs(p.y) < 0.75) {
					state.tracer_mass_kg_m2[1][i] += dry * (2.0e-4 * (1.0 - 0.5 * vertical));
				}
			}
		}

		const double water_before_adjust = total_water(core, state);
		const auto initial_adjust = moist.saturation_adjust(state);
		require(initial_adjust.condensed_water_kg > 0.0,
			"coupled moist initialization exercised no condensation");
		require(initial_adjust.evaporated_water_kg > 0.0,
			"coupled moist initialization exercised no evaporation");
		require(initial_adjust.max_relative_humidity_after <= 1.00000002,
			"initial coupled saturation adjustment left supersaturation");
		require(relative_error(total_water(core, state), water_before_adjust) < 5e-12,
			"initial coupled saturation adjustment changed total water");

		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.018 * grid.cell(c).center.z);
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				state.layer_mass_kg_m2[i] *= factor;
				state.theta_mass_kg_k_m2[i] *= factor;
				for (auto &tracer : state.tracer_mass_kg_m2) tracer[i] *= factor;
			}
		}

		const double water0 = total_water(core, state);
		const double dry_mass0 = core.total_dry_mass_kg(state);
		const double duration = 2.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		int remaps = 0;
		int source_active_steps = 0;
		double max_water_drift = 0.0;
		double max_rh_after = 0.0;
		double max_abs_dt = 0.0;
		double cumulative_condensed = initial_adjust.condensed_water_kg;
		double cumulative_evaporated = initial_adjust.evaporated_water_kg;
		double max_wind = 0.0;
		double min_temperature = std::numeric_limits<double>::infinity();
		double max_temperature = -std::numeric_limits<double>::infinity();

		while (elapsed < duration) {
			const double request = std::min(600.0, duration - elapsed);
			const auto dry_diag = core.step(state, request, 0.28);
			require(dry_diag.accepted_dt_s > 0.0,
				"coupled moist dry-core timestep collapsed");
			require(dry_diag.max_courant <= 0.2800000001,
				"coupled moist dry-core exceeded CFL target");
			require(dry_diag.max_relative_tracer_mass_error < 3e-10,
				"coupled moist transport drifted water tracers");
			require(dry_diag.max_coordinate_column_tracer_mass_error < 2e-11,
				"coupled moist remap changed a column tracer budget");
			if (dry_diag.coordinate_remap_applied) ++remaps;

			const auto source_diag = moist.saturation_adjust(state);
			require(source_diag.relative_total_water_error < 5e-12,
				"coupled moist source changed total water");
			require(source_diag.max_relative_cell_water_error < 5e-12,
				"coupled moist source changed local total water");
			require(source_diag.max_specific_enthalpy_error_j_kg < 1e-4,
				"coupled moist source violated enthalpy gate");
			require(source_diag.max_relative_humidity_after <= 1.00000002,
				"coupled moist source left supersaturation");
			if (source_diag.condensed_water_kg > 0.0 || source_diag.evaporated_water_kg > 0.0) {
				++source_active_steps;
			}
			cumulative_condensed += source_diag.condensed_water_kg;
			cumulative_evaporated += source_diag.evaporated_water_kg;
			max_rh_after = std::max(max_rh_after, source_diag.max_relative_humidity_after);
			max_abs_dt = std::max(max_abs_dt, source_diag.max_abs_temperature_change_k);
			min_temperature = std::min(min_temperature, source_diag.min_temperature_k);
			max_temperature = std::max(max_temperature, source_diag.max_temperature_k);
			max_wind = std::max(max_wind, dry_diag.max_speed_mps);

			require_water_nonnegative(state);
			const double drift = relative_error(total_water(core, state), water0);
			max_water_drift = std::max(max_water_drift, drift);
			require(drift < 8e-9,
				"coupled moist cycle accumulated excessive total-water drift");
			elapsed += dry_diag.accepted_dt_s;
			++steps;
			require(steps < 2000, "coupled moist cycle used excessive timesteps");
		}

		require(remaps > 0,
			"coupled moist cycle never exercised coordinate remapping");
		require(source_active_steps > 0,
			"coupled moist cycle never required phase adjustment after transport");
		require(cumulative_condensed > 0.0 && cumulative_evaporated > 0.0,
			"coupled moist cycle did not exercise both phase directions");
		require(relative_error(core.total_dry_mass_kg(state), dry_mass0) < 5e-9,
			"coupled moist cycle drifted dry mass");
		require(relative_error(total_water(core, state), water0) < 8e-9,
			"coupled moist cycle drifted total water");
		require(max_relative_humidity(core, state) <= 1.00000003,
			"final coupled moist state is supersaturated");
		require(min_temperature > 180.0 && max_temperature < 330.0,
			"coupled moist cycle developed unreasonable temperature");
		require(max_wind < 250.0,
			"coupled moist cycle developed runaway wind");

		std::cout << "VoronoiMoistCycle PASS\n"
			<< "  2h steps/remaps/source-active: " << steps << "/" << remaps
			<< "/" << source_active_steps << "\n"
			<< "  cumulative condensed/evaporated: " << cumulative_condensed
			<< "/" << cumulative_evaporated << " kg\n"
			<< "  max total-water drift: " << max_water_drift << "\n"
			<< "  max RH after adjustment: " << max_rh_after << "\n"
			<< "  max source |dT| / wind: " << max_abs_dt << "/" << max_wind << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistCycle FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
