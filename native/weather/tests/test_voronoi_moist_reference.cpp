#include "voronoi_moist_reference.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistReference;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 278.0;
constexpr double RH = 0.70;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(6, R);
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			2.0 * 3.14159265358979323846 / (11.5 * 3600.0), {0.0, 1.0, 0.0});
		std::vector<double> terrain(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &p = grid.cell(c).center;
			terrain[static_cast<size_t>(c)] = 900.0 * p.x + 350.0 * p.z + 150.0 * p.x * p.y;
		}
		core.set_surface_height_m(terrain);
		VoronoiMoistReference reference(core);
		VoronoiMoistReference::Diagnostics d;
		auto state = reference.make_isothermal_terrain_balanced(PS, T, RH, &d);

		require(d.max_coordinate_mass_fraction_error < 2.0e-12,
			"moist terrain reference is not aligned with the dry-mass coordinate");
		require(d.max_relative_humidity_error < 5.0e-10,
			"moist terrain reference did not close on requested relative humidity");
		require(d.max_pressure_acceleration_mps2 < 3.0e-5,
			"moist terrain reference has excessive startup pressure force");
		require(d.min_surface_pressure_pa > core.transport().top_pressure_pa()
				&& d.max_surface_pressure_pa > d.min_surface_pressure_pa,
			"moist terrain reference has invalid terrain pressure range");

		VoronoiMoistThermodynamics moist(core.transport());
		const auto thermo = moist.diagnose_thermodynamics(state);
		for (double temp : thermo.temperature_k) {
			require(std::abs(temp - T) < 2.0e-9,
				"moist terrain reference is not isothermal under full-pressure theta/T");
		}

		// The reference should remain quiet when handed directly to the same
		// production moist-pressure SSPRK/remap path.
		core.set_moist_pressure_feedback(true);
		const auto step = core.step(state, 60.0, 0.28);
		require(step.accepted_dt_s > 0.0,
			"moist terrain reference was rejected by the production core");
		require(step.max_speed_mps < 0.01,
			"moist terrain reference generated excessive startup wind");
		require(step.max_coordinate_mass_fraction_error < 2.0e-12,
			"moist terrain reference lost coordinate alignment on first step");

		// RH=0 must remain a valid dry-limit balanced construction on the same
		// terrain, with five zero water tracers ready for the moist runtime path.
		VoronoiMoistReference::Diagnostics dry_d;
		const auto dry_limit = reference.make_isothermal_terrain_balanced(PS, T, 0.0, &dry_d);
		require(dry_d.max_pressure_acceleration_mps2 < 2.0e-9,
			"zero-humidity moist reference does not recover dry terrain balance");
		for (const auto &tracer : dry_limit.tracer_mass_kg_m2) {
			for (double mass : tracer) require(mass == 0.0,
				"zero-humidity moist reference contains water mass");
		}

		std::cout << "VoronoiMoistReference PASS\n"
			<< "  max RH error: " << d.max_relative_humidity_error << "\n"
			<< "  coordinate error: " << d.max_coordinate_mass_fraction_error << "\n"
			<< "  initial pressure acceleration: " << d.max_pressure_acceleration_mps2 << " m/s2\n"
			<< "  first-step max wind: " << step.max_speed_mps << " m/s\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistReference FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
