#include "voronoi_moist_energy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistEnergyDiagnostics;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;
constexpr double PS = 110000.0;
constexpr double T = 285.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max({std::abs(a), std::abs(b), 1.0});
}
} // namespace

int main() {
	try {
		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryCore core(grid, 9.80665, 8000.0, 7500.0,
			2.0 * 3.14159265358979323846 / (11.5 * 3600.0), {0.0, 1.0, 0.0});
		auto dry = core.make_isothermal_reference(PS, T);
		VoronoiMoistThermodynamics moist(core.transport());
		moist.ensure_water_tracers(dry);
		VoronoiMoistEnergyDiagnostics energy(core);

		const double dry_reference = core.total_dry_energy_j(dry);
		const auto dry_diag = energy.diagnose(dry);
		const double dry_limit_error = relative_error(dry_diag.total_energy_j, dry_reference);
		require(dry_limit_error < 2.0e-13,
			"moist dynamical energy does not reduce to the dry energy diagnostic");
		require(dry_diag.latent_reference_energy_j == 0.0,
			"zero-water dry limit has non-zero latent-reference energy");

		auto humid = core.make_isothermal_reference(PS, T);
		moist.initialize_uniform_relative_humidity(humid, 0.70);
		// Add suspended liquid/ice loading without changing theta to exercise all
		// moist energy components; rain/snow remain zero here.
		for (size_t i = 0; i < humid.layer_mass_kg_m2.size(); ++i) {
			const double m = humid.layer_mass_kg_m2[i];
			humid.tracer_mass_kg_m2[1][i] = 1.0e-4 * m;
			humid.tracer_mass_kg_m2[2][i] = 5.0e-5 * m;
		}
		const auto humid_diag = energy.diagnose(humid);
		require(std::isfinite(humid_diag.total_energy_j) && humid_diag.total_energy_j > 0.0,
			"moist dynamical energy is non-finite or non-positive");
		require(humid_diag.latent_reference_energy_j != 0.0,
			"humid state produced no latent-reference contribution");
		require(humid_diag.potential_energy_j > 0.0,
			"humid state produced no potential-energy contribution");

		// The diagnostic is read-only.
		const auto humid_before = humid;
		(void)energy.diagnose(humid);
		require(humid.layer_mass_kg_m2 == humid_before.layer_mass_kg_m2
				&& humid.theta_mass_kg_k_m2 == humid_before.theta_mass_kg_k_m2
				&& humid.edge_normal_mps == humid_before.edge_normal_mps
				&& humid.tracer_mass_kg_m2 == humid_before.tracer_mass_kg_m2,
			"moist energy diagnostic mutated prognostic state");

		std::cout << "VoronoiMoistEnergy PASS\n"
			<< "  dry-limit relative error: " << dry_limit_error << "\n"
			<< "  humid total energy: " << humid_diag.total_energy_j << " J\n"
			<< "  humid latent reference: " << humid_diag.latent_reference_energy_j << " J\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistEnergy FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
