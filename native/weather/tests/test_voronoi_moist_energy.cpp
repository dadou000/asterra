#include "voronoi_moist_energy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistEnergyDiagnostics;
using asterra::weather::VoronoiMoistThermodynamics;
using asterra::weather::cross;
using asterra::weather::dot;

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

		// Add a nonzero analytic wind so the dry-limit check also exercises kinetic
		// energy and both forms of angular momentum rather than only rest values.
		const Vec3d axis{0.0, 1.0, 0.0};
		constexpr double U0 = 35.0;
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			for (int e = 0; e < grid.edge_count(); ++e) {
				const auto &edge = grid.edge(e);
				const Vec3d analytic = cross(axis, edge.midpoint) * U0;
				dry.edge_normal_mps[static_cast<size_t>(k * grid.edge_count() + e)]
					= dot(analytic, edge.normal_a_to_b);
			}
		}

		const double dry_reference = core.total_dry_energy_j(dry);
		const double dry_relative_aam =
			core.total_relative_axial_angular_momentum_kg_m2_s(dry);
		const double dry_absolute_aam =
			core.total_absolute_axial_angular_momentum_kg_m2_s(dry);
		const auto dry_diag = energy.diagnose(dry);
		const double dry_limit_error = relative_error(dry_diag.total_energy_j, dry_reference);
		const double relative_aam_error = relative_error(
			dry_diag.relative_axial_angular_momentum_kg_m2_s, dry_relative_aam);
		const double absolute_aam_error = relative_error(
			dry_diag.absolute_axial_angular_momentum_kg_m2_s, dry_absolute_aam);
		require(dry_limit_error < 2.0e-13,
			"moist dynamical energy does not reduce to the dry energy diagnostic");
		require(relative_aam_error < 2.0e-13 && absolute_aam_error < 2.0e-13,
			"moist angular momentum does not reduce to dry AAM in the zero-water limit");
		require(dry_diag.latent_reference_energy_j == 0.0,
			"zero-water dry limit has non-zero latent-reference energy");

		auto humid = core.make_isothermal_reference(PS, T);
		moist.initialize_uniform_relative_humidity(humid, 0.70);
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
		require(std::isfinite(humid_diag.absolute_axial_angular_momentum_kg_m2_s),
			"humid state produced invalid absolute angular momentum");

		const auto humid_before = humid;
		(void)energy.diagnose(humid);
		require(humid.layer_mass_kg_m2 == humid_before.layer_mass_kg_m2
				&& humid.theta_mass_kg_k_m2 == humid_before.theta_mass_kg_k_m2
				&& humid.edge_normal_mps == humid_before.edge_normal_mps
				&& humid.tracer_mass_kg_m2 == humid_before.tracer_mass_kg_m2,
			"moist budget diagnostic mutated prognostic state");

		std::cout << "VoronoiMoistEnergy PASS\n"
			<< "  dry energy/AAM limit errors: " << dry_limit_error << "/"
			<< relative_aam_error << "/" << absolute_aam_error << "\n"
			<< "  humid total energy: " << humid_diag.total_energy_j << " J\n"
			<< "  humid latent reference: " << humid_diag.latent_reference_energy_j << " J\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistEnergy FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}
