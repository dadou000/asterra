#include "voronoi_dry_core.h"
#include "voronoi_moist_thermodynamics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T0 = 288.0;
constexpr double G = 9.80665;

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

		std::cout << "VoronoiMoistPressureFeedback PASS\n"
			<< "  dry/moist CFL: " << dry_cfl << "/" << moist_cfl << "\n"
			<< "  gradient max pressure accel: "
			<< gradient_step.max_pressure_acceleration_mps2 << " m/s2\n"
			<< "  gradient max speed: " << gradient_step.max_speed_mps << " m/s\n"
			<< "  tracer stage change: " << tracer_change << " kg/m2\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistPressureFeedback FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
