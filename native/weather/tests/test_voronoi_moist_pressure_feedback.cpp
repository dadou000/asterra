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
constexpr double T0 = 288.0;
constexpr double G = 9.80665;
constexpr double OMEGA = 2.0 * PI / (11.5 * 3600.0);

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}

double max_abs_difference(const std::vector<double> &a,
		const std::vector<double> &b) {
	require(a.size() == b.size(), "state arrays have different sizes");
	double maximum = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		maximum = std::max(maximum, std::abs(a[i] - b[i]));
	}
	return maximum;
}

bool state_exact_equal(const VoronoiDryCore::State &a,
		const VoronoiDryCore::State &b) {
	return a.layer_mass_kg_m2 == b.layer_mass_kg_m2
		&& a.theta_mass_kg_k_m2 == b.theta_mass_kg_k_m2
		&& a.edge_normal_mps == b.edge_normal_mps
		&& a.tracer_mass_kg_m2 == b.tracer_mass_kg_m2;
}

double total_water_kg(const VoronoiDryCore &core,
		const VoronoiDryCore::State &state) {
	if (state.tracer_mass_kg_m2.size() < 3) return 0.0;
	return core.total_tracer_mass_kg(state, 0)
		+ core.total_tracer_mass_kg(state, 1)
		+ core.total_tracer_mass_kg(state, 2);
}

} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryCore dry_core(grid, G, 8000.0, 7500.0, 0.0);
		VoronoiDryCore moist_core(grid, G, 8000.0, 7500.0, 0.0);
		moist_core.set_moist_pressure_feedback(true);

		// Zero-water dry limit: turning the moist pressure path on must reproduce
		// the established dry dynamics for a genuinely pressure-driven state.
		auto dry_state = dry_core.make_isothermal_reference(PS, T0);
		VoronoiMoistThermodynamics dry_thermo(dry_core.transport());
		dry_thermo.ensure_water_tracers(dry_state);
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			for (int c = 0; c < grid.cell_count(); ++c) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const double factor = 1.0 + 0.008 * grid.cell(c).center.x;
				dry_state.layer_mass_kg_m2[i] *= factor;
				dry_state.theta_mass_kg_k_m2[i] *= factor;
			}
		}
		auto moist_dry_state = dry_state;
		const auto dry_step = dry_core.step(dry_state, 60.0, 0.24, 10);
		const auto moist_dry_step = moist_core.step(moist_dry_state, 60.0, 0.24, 10);
		require(dry_step.accepted_dt_s > 0.0 && moist_dry_step.accepted_dt_s > 0.0,
			"dry-limit comparison failed to accept a timestep");
		require(std::abs(dry_step.accepted_dt_s - moist_dry_step.accepted_dt_s) < 1e-12,
			"moist dry-limit changed adaptive timestep");
		require(max_abs_difference(dry_state.layer_mass_kg_m2,
			moist_dry_state.layer_mass_kg_m2) < 1e-10,
			"moist feedback does not recover dry mass evolution in zero-water limit");
		require(max_abs_difference(dry_state.theta_mass_kg_k_m2,
			moist_dry_state.theta_mass_kg_k_m2) < 1e-7,
			"moist feedback does not recover dry theta evolution in zero-water limit");
		require(max_abs_difference(dry_state.edge_normal_mps,
			moist_dry_state.edge_normal_mps) < 1e-12,
			"moist feedback does not recover dry momentum in zero-water limit");

		// Uniform moisture is horizontally balanced. It changes virtual temperature
		// and therefore the acoustic/CFL estimate, but must not generate wind.
		auto uniform = moist_core.make_isothermal_reference(PS, T0);
		VoronoiMoistThermodynamics moist_thermo(moist_core.transport());
		moist_thermo.ensure_water_tracers(uniform);
		constexpr double QV_UNIFORM = 0.012;
		for (size_t i = 0; i < uniform.layer_mass_kg_m2.size(); ++i) {
			uniform.tracer_mass_kg_m2[0][i] = QV_UNIFORM * uniform.layer_mass_kg_m2[i];
		}
		const double dry_cfl = dry_core.dynamics().max_courant(uniform, 60.0);
		const double moist_cfl = moist_core.dynamics().max_courant(uniform, 60.0);
		require(moist_cfl > dry_cfl,
			"moist pressure feedback did not increase virtual-temperature wave CFL");
		const auto uniform_before = uniform;
		const auto uniform_step = moist_core.step(uniform, 300.0, 0.24, 10);
		require(uniform_step.accepted_dt_s > 0.0,
			"uniform moist rest state rejected timestep");
		require(uniform_step.max_pressure_acceleration_mps2 < 1e-13,
			"uniform moisture generated horizontal pressure force");
		require(state_exact_equal(uniform, uniform_before),
			"uniform moist hydrostatic rest state was not an exact no-op");

		// A humidity gradient at otherwise identical dry mass/theta is invisible to
		// the old dry force but must accelerate the moist core. Starting from zero
		// wind also proves the pressure force is evaluated inside SSPRK: stage 1
		// creates velocity, and later stages must already transport the tracer.
		auto gradient = moist_core.make_isothermal_reference(PS, T0);
		moist_thermo.ensure_water_tracers(gradient);
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			for (int c = 0; c < grid.cell_count(); ++c) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				const double qv = 0.008 + 0.004 * grid.cell(c).center.x;
				gradient.tracer_mass_kg_m2[0][i] = qv * gradient.layer_mass_kg_m2[i];
			}
		}
		const auto gradient_before = gradient;
		const double water_before = moist_core.total_tracer_mass_kg(gradient, 0);
		const auto gradient_step = moist_core.step(gradient, 90.0, 0.24, 10);
		require(gradient_step.accepted_dt_s > 0.0,
			"moist pressure-gradient state rejected timestep");
		require(gradient_step.max_pressure_acceleration_mps2 > 1e-8,
			"humidity gradient produced no momentum pressure force");
		require(gradient_step.max_speed_mps > 0.0,
			"humidity gradient did not generate wind");
		const double tracer_change = max_abs_difference(
			gradient.tracer_mass_kg_m2[0], gradient_before.tracer_mass_kg_m2[0]);
		require(tracer_change > 1e-12,
			"humidity tracer remained frozen; pressure feedback may be outside SSPRK stages");
		const double water_after = moist_core.total_tracer_mass_kg(gradient, 0);
		require(relative_error(water_after, water_before) < 1e-10,
			"moist pressure feedback broke conservative tracer transport");
		require(gradient_step.relative_dry_mass_error < 1e-10,
			"moist pressure feedback broke dry-mass conservation");
		require(gradient_step.max_coordinate_mass_fraction_error < 1e-11,
			"moist pressure feedback left the accepted state off the vertical coordinate");

		// The same moisture gradient with feedback disabled is exactly invisible to
		// the dry pressure operator and therefore exercises the zero-force fast path.
		auto dry_gradient = gradient_before;
		const auto dry_gradient_before = dry_gradient;
		const auto dry_gradient_step = dry_core.step(dry_gradient, 90.0, 0.24, 10);
		require(dry_gradient_step.accepted_dt_s == 90.0,
			"dry moisture-gradient control unexpectedly reduced timestep");
		require(dry_gradient_step.max_pressure_acceleration_mps2 == 0.0,
			"dry pressure operator reacted to passive water tracer");
		require(state_exact_equal(dry_gradient, dry_gradient_before),
			"dry pressure operator moved a passive moisture-gradient rest state");

		// Multi-hour characterization with rotation and repeated saturation
		// adjustment. This is deliberately a closed atmosphere: pressure feedback
		// may redistribute water/heat, but it must not create dry mass or water and
		// the accepted state must remain positive and on the vertical coordinate.
		VoronoiDryCore long_core(grid, G, 8000.0, 7500.0,
			OMEGA, {0.0, 1.0, 0.0});
		long_core.set_moist_pressure_feedback(true);
		auto long_state = long_core.make_isothermal_reference(PS, T0);
		VoronoiMoistThermodynamics long_thermo(long_core.transport());
		long_thermo.initialize_uniform_relative_humidity(long_state, 0.78);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double factor = std::exp(0.012 * grid.cell(c).center.z);
			for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
				const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
				long_state.layer_mass_kg_m2[i] *= factor;
				long_state.theta_mass_kg_k_m2[i] *= factor;
				for (auto &tracer : long_state.tracer_mass_kg_m2) tracer[i] *= factor;
			}
		}
		(void)long_thermo.saturation_adjust(long_state);
		const double long_dry_before = long_core.total_dry_mass_kg(long_state);
		const double long_water_before = total_water_kg(long_core, long_state);
		constexpr double DURATION = 3.0 * 3600.0;
		double elapsed = 0.0;
		int steps = 0;
		int remaps = 0;
		double max_long_speed = 0.0;
		double max_long_pressure_accel = 0.0;
		double max_long_water_drift = 0.0;
		double min_temperature = std::numeric_limits<double>::infinity();
		double max_temperature = -std::numeric_limits<double>::infinity();
		while (elapsed < DURATION) {
			const double request = std::min(300.0, DURATION - elapsed);
			const auto d = long_core.step(long_state, request, 0.25, 10);
			require(d.accepted_dt_s > 0.0,
				"multi-hour moist pressure-feedback timestep collapsed");
			require(d.max_courant <= 0.2500000001,
				"multi-hour moist pressure-feedback run exceeded CFL target");
			require(d.relative_dry_mass_error < 2e-10,
				"multi-hour moist pressure-feedback step drifted in dry mass");
			require(d.max_relative_tracer_mass_error < 2e-10,
				"multi-hour moist pressure-feedback step drifted in water tracers");
			require(d.max_coordinate_mass_fraction_error < 2e-11,
				"multi-hour moist pressure-feedback run left vertical coordinate");
			if (d.coordinate_remap_applied) ++remaps;
			max_long_speed = std::max(max_long_speed, d.max_speed_mps);
			max_long_pressure_accel = std::max(max_long_pressure_accel,
				d.max_pressure_acceleration_mps2);

			const auto phase = long_thermo.saturation_adjust(long_state);
			require(phase.relative_total_water_error < 5e-12,
				"multi-hour saturation adjustment drifted in total water");
			require(phase.max_relative_humidity_after <= 1.00000001,
				"multi-hour saturation adjustment left supersaturation");
			min_temperature = std::min(min_temperature, phase.min_temperature_k);
			max_temperature = std::max(max_temperature, phase.max_temperature_k);
			const double current_water = total_water_kg(long_core, long_state);
			max_long_water_drift = std::max(max_long_water_drift,
				relative_error(current_water, long_water_before));
			for (const auto &tracer : long_state.tracer_mass_kg_m2) {
				for (double mass : tracer) {
					require(std::isfinite(mass) && mass >= 0.0,
						"multi-hour moist pressure feedback produced invalid water mass");
				}
			}
			elapsed += d.accepted_dt_s;
			++steps;
			require(steps < 2000,
				"multi-hour moist pressure-feedback run used excessive timesteps");
		}
		require(remaps > 0,
			"multi-hour moist pressure-feedback run never exercised coordinate remap");
		require(relative_error(long_core.total_dry_mass_kg(long_state), long_dry_before) < 2e-8,
			"multi-hour moist pressure-feedback run drifted in global dry mass");
		require(relative_error(total_water_kg(long_core, long_state), long_water_before) < 2e-8,
			"multi-hour moist pressure-feedback run drifted in global water");
		require(max_long_water_drift < 2e-8,
			"multi-hour moist pressure-feedback water budget wandered excessively");
		require(std::isfinite(max_long_speed) && max_long_speed < 200.0,
			"multi-hour moist pressure-feedback winds became unbounded");
		require(std::isfinite(min_temperature) && std::isfinite(max_temperature)
				&& min_temperature > 150.0 && max_temperature < 360.0,
			"multi-hour moist pressure-feedback temperature became unphysical");

		std::cout << "VoronoiMoistPressureFeedback PASS\n"
			<< "  dry/moist CFL: " << dry_cfl << "/" << moist_cfl << "\n"
			<< "  gradient max pressure accel: "
			<< gradient_step.max_pressure_acceleration_mps2 << " m/s2\n"
			<< "  gradient max speed: " << gradient_step.max_speed_mps << " m/s\n"
			<< "  tracer stage change: " << tracer_change << " kg/m2\n"
			<< "  3h steps/remaps: " << steps << "/" << remaps << "\n"
			<< "  3h max speed/pressure accel: " << max_long_speed << "/"
			<< max_long_pressure_accel << "\n"
			<< "  3h T range: " << min_temperature << ".." << max_temperature << " K\n"
			<< "  3h max water drift: " << max_long_water_drift << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistPressureFeedback FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
